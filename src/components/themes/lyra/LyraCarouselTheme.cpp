#include "LyraCarouselTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/cover.h"
#include "components/icons/folder.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "fontIds.h"

const ThemeMetrics& LyraCarouselTheme::themeMetrics() const {
#if FREEINK_DEVICE_X4PRO
  // See BaseTheme::themeMetrics().
  return SETTINGS.fullTouchUi ? LyraCarouselMetrics::values : LyraCarouselMetrics::noActionBarValues;
#else
  return LyraCarouselMetrics::values;
#endif
}

namespace {

constexpr int kTitleFontId = UI_12_FONT_ID;
constexpr int kFooterFontId = UI_10_FONT_ID;
constexpr int kMenuLabelFontId = SMALL_FONT_ID;

// Cover strip
constexpr int kTitleTopClearance = 4;
constexpr int kTitleBottomGap = 8;
constexpr int kTitleLines = 2;
constexpr int kCornerRadius = 6;
constexpr int kSideOutlineW = 2;
constexpr int kThinOutlineW = 1;    // centre cover is always outlined
constexpr int kSelectionLineW = 3;  // ...thicker while the carousel row is active
constexpr int kCenterRingW = 4;     // white ring separating centre from side art
constexpr int kNearOverlap = 4;
constexpr int kFarOverlap = 2;
constexpr int kNearCoverInset = 10;

// Dot row + progress footer
constexpr int kDotSize = 8;
constexpr int kDotGap = 6;
constexpr int kDotsTopGap = 8;
constexpr int kFooterTopGap = 10;
constexpr int kFooterBarHeight = 5;
constexpr int kFooterPercentTopGap = 2;

// Icon menu row, anchored to the screen bottom rather than the rect it is
// handed: the carousel owns the whole strip above it.
constexpr int kMenuIconSize = 32;  // must match the bitmap dimensions; drawIcon does not scale
constexpr int kMenuIconPad = 14;   // symmetric, so tile height is 60
constexpr int kMenuHighlightPad = 7;
constexpr int kMenuLabelTopGap = 3;
constexpr int kMenuBottomGap = 8;
constexpr int kMenuLabelSidePadding = 20;

// Which book sits in the centre, remembered across renders so that navigating
// the icon row keeps showing the carousel where the user left it, and so that
// hitTestRecentBookCover can map a tapped slot back to a book index (its
// signature carries no centre index). Single-threaded: only the UI task draws
// or hit-tests the home screen, so no atomic is needed.
int lastCenterIdx = -1;

// The centre artwork rect as last painted. Letterboxing makes it depend on the
// cover's own aspect, so the selection outline needs it on the frames that
// reuse the snapshot rather than redrawing -- and re-reading the thumbnail off
// SD just to recompute it would undo the point of the snapshot.
Rect lastCenterDrawn{0, 0, 0, 0};

// Slot rects as last drawn, in hit-test priority order (centre first, then the
// near covers, then the far ones) so an overlapping tap resolves to the cover
// painted on top. hitTestRecentBookCover is handed no renderer, and the draw
// always precedes any tap on what it drew, so recording the geometry here is
// the only way to keep hit-testing in lockstep with it.
constexpr int kMaxVisibleSlots = 5;
struct CoverSlot {
  Rect rect;
  int bookIdx;
};
CoverSlot drawnSlots[kMaxVisibleSlots];
int drawnSlotCount = 0;

void resetDrawnSlots() { drawnSlotCount = 0; }

void recordDrawnSlot(const Rect& rect, int bookIdx) {
  if (drawnSlotCount >= kMaxVisibleSlots || rect.width <= 0 || rect.height <= 0) return;
  for (int i = 0; i < drawnSlotCount; ++i) {
    if (drawnSlots[i].bookIdx == bookIdx) return;  // already covered by a nearer slot
  }
  drawnSlots[drawnSlotCount++] = CoverSlot{rect, bookIdx};
}

// The menu row as last drawn, for hitTestButtonMenu — which, like the cover
// hit test, is handed no renderer and so cannot recompute the layout itself.
struct MenuBand {
  int y;
  int height;
  int tileW;
  int count;
};
MenuBand drawnMenu{0, 0, 0, 0};

struct MenuLayout {
  int tileH;
  int tileW;
  int labelLineHeight;
  int rowY;
  int labelY;
};

MenuLayout computeMenuLayout(const GfxRenderer& renderer, const ThemeMetrics& m, int buttonCount) {
  const int tileH = kMenuIconPad + kMenuIconSize + kMenuIconPad;
  const int labelLineHeight = renderer.getLineHeight(kMenuLabelFontId);
  const int rowY = renderer.getScreenHeight() - m.buttonHintsHeight - kMenuBottomGap - tileH;
  return MenuLayout{tileH, renderer.getScreenWidth() / std::max(1, buttonCount), labelLineHeight, rowY,
                    rowY - kMenuLabelTopGap - labelLineHeight};
}

// Our 32 px icon set. LyraTheme's iconForName lives in its own anonymous
// namespace and is not reachable from here; only the icons the home menu can
// actually show are mapped.
const uint8_t* menuIcon(UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder:
      return FolderIcon;
    case UIIcon::Book:
      return BookIcon;
    case UIIcon::Recent:
      return RecentIcon;
    case UIIcon::Settings:
      return Settings2Icon;
    case UIIcon::Transfer:
      return TransferIcon;
    case UIIcon::Library:
      return LibraryIcon;
    default:
      return nullptr;
  }
}

// Largest rect fitting srcW x srcH inside box at the source's own aspect,
// centred, and never scaled past the source's own pixels. The no-upscale cap is
// deliberate: a small or odd-shaped cover is shown at its true size rather than
// blown up to fill the slot.
Rect fitInside(int srcW, int srcH, const Rect& box) {
  if (srcW <= 0 || srcH <= 0 || box.width <= 0 || box.height <= 0) {
    return Rect{box.x, box.y, 0, 0};
  }
  int w = srcW;
  int h = srcH;
  if (w > box.width) {
    h = static_cast<int>(static_cast<int64_t>(h) * box.width / w);
    w = box.width;
  }
  if (h > box.height) {
    w = static_cast<int>(static_cast<int64_t>(w) * box.height / h);
    h = box.height;
  }
  w = std::max(1, w);
  h = std::max(1, h);
  return Rect{box.x + (box.width - w) / 2, box.y + (box.height - h) / 2, w, h};
}

// The centre cover's nominal box. Independent of the artwork so the title, dot
// row and footer keep a stable position as the carousel rotates.
Rect centerBox(const GfxRenderer& renderer, const Rect& rect) {
  const int titleLineHeight = renderer.getLineHeight(kTitleFontId);
  const int y = rect.y + kTitleTopClearance + titleLineHeight * kTitleLines + kTitleBottomGap;
  return Rect{(renderer.getScreenWidth() - LyraCarouselTheme::kCenterBoxW) / 2, y, LyraCarouselTheme::kCenterBoxW,
              LyraCarouselTheme::kCenterBoxH};
}

// Horizontal placement of the four side slots around the centre box.
struct SideSlots {
  int leftFarX;
  int leftNearX;
  int rightNearX;
  int rightFarX;
  int tileY;
};

SideSlots computeSideSlots(const GfxRenderer& renderer, const Rect& center) {
  const int screenW = renderer.getScreenWidth();
  const int sideMaxH = std::max(LyraCarouselTheme::kNearSideInnerH, LyraCarouselTheme::kNearSideOuterH);
  const int baseLeftNearX = center.x - LyraCarouselTheme::kNearSideW + kNearOverlap;
  const int baseRightNearX = center.x + center.width - kNearOverlap;
  return SideSlots{
      std::max(0, baseLeftNearX - LyraCarouselTheme::kFarSideW + kFarOverlap),
      baseLeftNearX + kNearCoverInset,
      baseRightNearX - kNearCoverInset,
      std::min(screenW - LyraCarouselTheme::kFarSideW, baseRightNearX + LyraCarouselTheme::kNearSideW - kFarOverlap),
      center.y + (center.height - sideMaxH) / 2,
  };
}

void drawPerspectiveOutline(const GfxRenderer& renderer, int x, int y, int width, int leftHeight, int rightHeight) {
  const int maxHeight = std::max(leftHeight, rightHeight);
  const int topLeft = (maxHeight - leftHeight) / 2;
  const int topRight = (maxHeight - rightHeight) / 2;
  const int rightX = x + width - 1;

  renderer.drawLine(x, y + topLeft, rightX, y + topRight, kSideOutlineW, true);
  renderer.drawLine(x, y + topLeft + leftHeight - 1, rightX, y + topRight + rightHeight - 1, kSideOutlineW, true);
  renderer.fillRect(x, y + topLeft, kSideOutlineW, leftHeight, true);
  renderer.fillRect(rightX - kSideOutlineW + 1, y + topRight, kSideOutlineW, rightHeight, true);
}

// Solid stand-in for a book with no usable artwork, in the same trapezoid.
void fillPerspectiveSilhouette(const GfxRenderer& renderer, int x, int y, int width, int leftHeight, int rightHeight) {
  const int maxHeight = std::max(leftHeight, rightHeight);
  renderer.fillRect(x, y, width, maxHeight, false);
  for (int dx = 0; dx < width; ++dx) {
    const int columnHeight = (width <= 1) ? leftHeight : (leftHeight + ((rightHeight - leftHeight) * dx) / (width - 1));
    renderer.fillRect(x + dx, y + (maxHeight - columnHeight) / 2, 1, columnHeight, true);
  }
}

}  // namespace

void LyraCarouselTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect,
                                            const std::vector<RecentBook>& recentBooks, const int selectorIndex,
                                            bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                                            std::function<bool()> storeCoverBuffer) const {
  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    return;
  }

  const int bookCount = static_cast<int>(recentBooks.size());
  // Navigating the icon row leaves the carousel where it was.
  const bool inCarouselRow = selectorIndex < bookCount;
  int centerIdx = inCarouselRow ? selectorIndex : (lastCenterIdx >= 0 ? lastCenterIdx : 0);
  if (centerIdx >= bookCount) centerIdx = bookCount - 1;

  // The snapshot in coverBuffer holds one carousel position; rotating away from
  // it invalidates the cache.
  if (centerIdx != lastCenterIdx) {
    coverRendered = false;
    coverBufferStored = false;
  }

  const Rect center = centerBox(renderer, rect);
  const SideSlots sides = computeSideSlots(renderer, center);

  // Fits a cover into the centre box at its true aspect. Falls back to a framed
  // placeholder when the book has no readable thumbnail.
  auto drawCenterCover = [&](int bookIdx) -> Rect {
    Rect drawn = center;
    const RecentBook& book = recentBooks[bookIdx];

    if (!book.coverBmpPath.empty()) {
      const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, themeMetrics().homeCoverHeight);
      FsFile file;
      if (Storage.openFileForRead("HOME", thumbPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
          drawn = fitInside(bitmap.getWidth(), bitmap.getHeight(), center);
          // White ring first so the side artwork never touches the centre edge.
          renderer.fillRect(drawn.x - kCenterRingW, drawn.y - kCenterRingW, drawn.width + 2 * kCenterRingW,
                            drawn.height + 2 * kCenterRingW, false);
          renderer.drawBitmap(bitmap, drawn.x, drawn.y, drawn.width, drawn.height);
          renderer.maskRoundedRectOutsideCorners(drawn.x, drawn.y, drawn.width, drawn.height, kCornerRadius,
                                                 Color::White);
          file.close();
          return drawn;
        }
        file.close();
      }
    }

    renderer.fillRect(drawn.x - kCenterRingW, drawn.y - kCenterRingW, drawn.width + 2 * kCenterRingW,
                      drawn.height + 2 * kCenterRingW, false);
    renderer.fillRoundedRect(drawn.x, drawn.y + drawn.height / 3, drawn.width, 2 * drawn.height / 3, kCornerRadius,
                             /*roundTopLeft=*/false, /*roundTopRight=*/false,
                             /*roundBottomLeft=*/true, /*roundBottomRight=*/true, Color::Black);
    renderer.drawIcon(CoverIcon, drawn.x + drawn.width / 2 - 16, drawn.y + drawn.height / 3 + 14, 32, 32);
    return drawn;
  };

  // Side covers keep their nominal trapezoid: the horizontal squeeze is the
  // perspective effect, so they are not letterboxed the way the centre is.
  auto drawSideCover = [&](int bookIdx, int x, int width, int leftHeight, int rightHeight) {
    const RecentBook& book = recentBooks[bookIdx];

    if (!book.coverBmpPath.empty()) {
      const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, themeMetrics().homeCoverHeight);
      FsFile file;
      if (Storage.openFileForRead("HOME", thumbPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderer.fillRect(x, sides.tileY, width, std::max(leftHeight, rightHeight), false);
          renderer.drawPerspectiveBitmap(bitmap, x, sides.tileY, width, leftHeight, rightHeight);
          file.close();
          drawPerspectiveOutline(renderer, x, sides.tileY, width, leftHeight, rightHeight);
          return;
        }
        file.close();
      }
    }

    fillPerspectiveSilhouette(renderer, x, sides.tileY, width, leftHeight, rightHeight);
  };

  Rect centerDrawn = center;

  // A stored snapshot that failed to restore leaves the strip blank, so treat
  // that the same as never having drawn it.
  if (!coverRendered || !bufferRestored) {
    // drawBitmap only sets black pixels, so the strip must be cleared or the
    // previous carousel position bleeds through.
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

    const int leftNearIdx = (centerIdx + bookCount - 1) % bookCount;
    const int leftFarIdx = (centerIdx + bookCount - 2) % bookCount;
    const int rightNearIdx = (centerIdx + 1) % bookCount;
    const int rightFarIdx = (centerIdx + 2) % bookCount;

    // Painted outermost first so each nearer cover overlaps the one behind it.
    if (bookCount >= 5) drawSideCover(leftFarIdx, sides.leftFarX, kFarSideW, kFarSideInnerH, kFarSideOuterH);
    if (bookCount >= 4) drawSideCover(rightFarIdx, sides.rightFarX, kFarSideW, kFarSideOuterH, kFarSideInnerH);
    if (bookCount >= 2) drawSideCover(leftNearIdx, sides.leftNearX, kNearSideW, kNearSideInnerH, kNearSideOuterH);
    if (bookCount >= 3) drawSideCover(rightNearIdx, sides.rightNearX, kNearSideW, kNearSideOuterH, kNearSideInnerH);

    centerDrawn = drawCenterCover(centerIdx);

    // Title, centred over the box rather than the artwork so it does not shift
    // as covers of different widths rotate through.
    const int titleMaxWidth =
        std::min(renderer.getScreenWidth() - 2 * themeMetrics().contentSidePadding, kCenterBoxW + 2 * kNearCoverInset);
    const auto titleLines = renderer.wrappedText(kTitleFontId, recentBooks[centerIdx].title.c_str(), titleMaxWidth,
                                                 kTitleLines, EpdFontFamily::BOLD);
    const int titleLineHeight = renderer.getLineHeight(kTitleFontId);
    const int titleCenterX = center.x + center.width / 2;
    int titleY = rect.y + kTitleTopClearance +
                 (titleLineHeight * kTitleLines - titleLineHeight * static_cast<int>(titleLines.size())) / 2;
    for (const auto& line : titleLines) {
      const int lineW = renderer.getTextWidth(kTitleFontId, line.c_str(), EpdFontFamily::BOLD);
      renderer.drawText(kTitleFontId, titleCenterX - lineW / 2, titleY, line.c_str(), true, EpdFontFamily::BOLD);
      titleY += titleLineHeight;
    }

    // Position dots, one per recent book, filled for the centred one.
    const int dotsY = center.y + center.height + kDotsTopGap;
    const int totalDotsW = bookCount * kDotSize + (bookCount - 1) * kDotGap;
    int dotX = center.x + (center.width - totalDotsW) / 2;
    for (int i = 0; i < bookCount; ++i) {
      if (i == centerIdx) {
        renderer.fillRect(dotX, dotsY, kDotSize, kDotSize, true);
      } else {
        renderer.drawRect(dotX, dotsY, kDotSize, kDotSize, true);
      }
      dotX += kDotSize + kDotGap;
    }

    // Progress footer for the centred book.
    const int8_t progress = recentBooks[centerIdx].progressPercent;
    if (progress >= 0) {
      const int barY = dotsY + kDotSize + kFooterTopGap;
      const int barWidth = center.width;
      const int barX = center.x;
      const int filledWidth = std::clamp(static_cast<int>(progress) * barWidth / 100, 0, barWidth);
      renderer.fillRectDither(barX, barY, barWidth, kFooterBarHeight, Color::LightGray);
      if (filledWidth > 0) {
        renderer.fillRect(barX, barY, filledWidth, kFooterBarHeight, true);
      }
      char progressLabel[8];
      snprintf(progressLabel, sizeof(progressLabel), "%d%%", static_cast<int>(progress));
      const int labelW = renderer.getTextWidth(kFooterFontId, progressLabel, EpdFontFamily::REGULAR);
      renderer.drawText(kFooterFontId, barX + barWidth - labelW, barY + kFooterBarHeight + kFooterPercentTopGap,
                        progressLabel, true, EpdFontFamily::REGULAR);
    }

    // Snapshot before the selection outline so moving between the carousel and
    // the icon row can restore the art and redraw the outline at its new
    // weight. A failed malloc just means the strip is redrawn next frame.
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
    lastCenterDrawn = centerDrawn;
  } else {
    // Snapshot restored the artwork; reuse the rect it was drawn at.
    centerDrawn = (lastCenterDrawn.width > 0 && lastCenterDrawn.height > 0) ? lastCenterDrawn : center;
  }

  lastCenterIdx = centerIdx;

  // Record what was painted, nearest-on-top first, for hitTestRecentBookCover.
  //
  // The centre slot registers as the whole nominal box, not the letterboxed
  // artwork rect: a cover that fits small leaves blank space inside a slot the
  // user can plainly see, and a tap or long press there must still land on the
  // book rather than falling into the dead zone between it and the side covers.
  resetDrawnSlots();
  recordDrawnSlot(center, centerIdx);
  const int sideMaxH = std::max(kNearSideInnerH, kNearSideOuterH);
  const int farMaxH = std::max(kFarSideInnerH, kFarSideOuterH);
  if (bookCount >= 2) {
    recordDrawnSlot(Rect{sides.leftNearX, sides.tileY, kNearSideW, sideMaxH}, (centerIdx + bookCount - 1) % bookCount);
  }
  if (bookCount >= 3) {
    recordDrawnSlot(Rect{sides.rightNearX, sides.tileY, kNearSideW, sideMaxH}, (centerIdx + 1) % bookCount);
  }
  if (bookCount >= 5) {
    recordDrawnSlot(Rect{sides.leftFarX, sides.tileY, kFarSideW, farMaxH}, (centerIdx + bookCount - 2) % bookCount);
  }
  if (bookCount >= 4) {
    recordDrawnSlot(Rect{sides.rightFarX, sides.tileY, kFarSideW, farMaxH}, (centerIdx + 2) % bookCount);
  }

  const int outlineW = inCarouselRow ? kSelectionLineW : kThinOutlineW;
  renderer.drawRoundedRect(centerDrawn.x, centerDrawn.y, centerDrawn.width, centerDrawn.height, outlineW, kCornerRadius,
                           true);
}

void LyraCarouselTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                       const std::function<std::string(int index)>& buttonLabel,
                                       const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rect;  // The carousel menu anchors to the screen bottom, not the rect.
  if (buttonCount <= 0) return;

  const MenuLayout layout = computeMenuLayout(renderer, themeMetrics(), buttonCount);
  // Whole strip is tappable, label line included.
  drawnMenu = MenuBand{layout.labelY, layout.rowY + layout.tileH - layout.labelY, layout.tileW, buttonCount};

  for (int i = 0; i < buttonCount; ++i) {
    const int iconX = i * layout.tileW + (layout.tileW - kMenuIconSize) / 2;
    const int iconY = layout.rowY + kMenuIconPad;

    if (selectedIndex == i) {
      const int plateSize = kMenuIconSize + 2 * kMenuHighlightPad;
      renderer.fillRoundedRect(iconX - kMenuHighlightPad, layout.rowY + (layout.tileH - plateSize) / 2, plateSize,
                               plateSize, kCornerRadius, Color::LightGray);
    }

    if (rowIcon != nullptr) {
      const uint8_t* icon = menuIcon(rowIcon(i));
      if (icon != nullptr) {
        renderer.drawIcon(icon, iconX, iconY, kMenuIconSize, kMenuIconSize);
      }
    }
  }

  // One shared label line under the row, naming only the selected tile.
  renderer.fillRect(0, layout.labelY, renderer.getScreenWidth(), layout.labelLineHeight, false);
  if (selectedIndex >= 0 && selectedIndex < buttonCount && buttonLabel != nullptr) {
    const std::string label = buttonLabel(selectedIndex);
    const std::string fitted =
        renderer.truncatedText(kMenuLabelFontId, label.c_str(), renderer.getScreenWidth() - 2 * kMenuLabelSidePadding);
    const int labelW = renderer.getTextWidth(kMenuLabelFontId, fitted.c_str(), EpdFontFamily::REGULAR);
    renderer.drawText(kMenuLabelFontId, (renderer.getScreenWidth() - labelW) / 2, layout.labelY, fitted.c_str(), true,
                      EpdFontFamily::REGULAR);
  }
}

int LyraCarouselTheme::hitTestRecentBookCover(const Rect& rect, const int slotCount, const int lx, const int ly) const {
  if (slotCount <= 0) return -1;
  if (lx < rect.x || lx >= rect.x + rect.width || ly < rect.y || ly >= rect.y + rect.height) return -1;

  for (int i = 0; i < drawnSlotCount; ++i) {
    const CoverSlot& slot = drawnSlots[i];
    if (lx >= slot.rect.x && lx < slot.rect.x + slot.rect.width && ly >= slot.rect.y &&
        ly < slot.rect.y + slot.rect.height) {
      return slot.bookIdx < slotCount ? slot.bookIdx : -1;
    }
  }
  return -1;
}

int LyraCarouselTheme::hitTestButtonMenu(const Rect& rect, const int buttonCount, const int lx, const int ly) const {
  (void)rect;  // drawButtonMenu anchors to the screen bottom, not to rect.
  if (buttonCount <= 0 || drawnMenu.count <= 0 || drawnMenu.tileW <= 0) return -1;
  if (ly < drawnMenu.y || ly >= drawnMenu.y + drawnMenu.height) return -1;
  if (lx < 0) return -1;

  const int index = lx / drawnMenu.tileW;
  if (index >= drawnMenu.count || index >= buttonCount) return -1;
  return index;
}
