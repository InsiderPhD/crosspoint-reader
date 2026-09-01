#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

int EpubReaderChapterSelectionActivity::getTotalItems() const { return epub->getTocItemsCount(); }

// Top edge of the first row's highlight band. Mirrors render()'s
// fillRect(..., 60 + contentY + row * 30 - 2, ..., 30): the band starts 2px
// above the text baseline row. Keep in sync with render() or taps land on the
// wrong row.
int EpubReaderChapterSelectionActivity::listTopY() const {
  const bool isPortraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  return 60 + hintGutterHeight - 2;
}

// The band the rows live in: below the title, above the hint row. Its bottom
// strip carries the page count, the same as on every other scrollable screen
// (BaseTheme::pageIndicatorRect).
Rect EpubReaderChapterSelectionActivity::listAreaRect() const {
  const bool isPortraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = 60 + hintGutterHeight;
  return Rect{0, startY, renderer.getScreenWidth(), renderer.getScreenHeight() - startY - ROW_H};
}

int EpubReaderChapterSelectionActivity::getPageItems() const {
  // Clamp to at least one item to avoid division by zero and empty paging.
  return std::max(1, GUI.contentHeightWithoutIndicator(listAreaRect()) / ROW_H);
}

void EpubReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  selectorIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (selectorIndex == -1) {
    selectorIndex = 0;
  }

  // Trigger first update
  requestUpdate();
}

void EpubReaderChapterSelectionActivity::onExit() { Activity::onExit(); }

// The Confirm short-press body, also fired by a Full Touch tap on the selected row.
void EpubReaderChapterSelectionActivity::activateSelectedChapter() {
  const auto newSpineIndex = epub->getSpineIndexForTocIndex(selectorIndex);
  if (newSpineIndex == -1) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  } else {
    setResult(ChapterResult{newSpineIndex});
    finish();
  }
}

void EpubReaderChapterSelectionActivity::loop() {
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

#if FREEINK_DEVICE_X4PRO
  // Full Touch: first tap on a row moves the cursor there; a second tap on the
  // already-selected row opens the chapter. Rows are page-relative, so the
  // tapped item is the current page start plus the row.
  if (SETTINGS.fullTouchUi) {
    int lx, ly;
    if (mappedInput.wasTapPoint(lx, ly)) {
      const int top = listTopY();
      const int row = (ly - top) / ROW_H;
      if (ly >= top && row >= 0 && row < pageItems) {
        const int pageStartIndex = selectorIndex / pageItems * pageItems;
        const int itemIndex = pageStartIndex + row;
        if (itemIndex < totalItems) {
          if (itemIndex != selectorIndex) {
            selectorIndex = itemIndex;
            requestUpdate();
          } else {
            activateSelectedChapter();
          }
          return;
        }
      }
      // Dead space (title bar, below the last row): no action.
    }
  }
#endif

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelectedChapter();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }

  buttonNavigator.onNextRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    requestUpdate();
  });

  // Full Touch: a vertical swipe turns a page, matching the held side key.
  if (TouchListNav::pageSwipe(mappedInput, totalItems, pageItems, selectorIndex)) {
    requestUpdate();
    return;
  }

  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });
}

void EpubReaderChapterSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  // Landscape orientation: reserve a horizontal gutter for button hints.
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Inverted portrait: reserve vertical space for hints at the top.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  // Manual centering to honor content gutters.
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_SELECT_CHAPTER), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_SELECT_CHAPTER), true, EpdFontFamily::BOLD);

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  // Row bands start at listTopY() (shared with the tap hit-testing in loop()).
  const int listTop = listTopY();
  // Highlight only the content area, not the hint gutters.
  renderer.fillRect(contentX, listTop + (selectorIndex % pageItems) * ROW_H, contentWidth - 1, ROW_H);

  for (int i = 0; i < pageItems; i++) {
    int itemIndex = pageStartIndex + i;
    if (itemIndex >= totalItems) break;
    const int displayY = listTop + 2 + i * ROW_H;
    const bool isSelected = (itemIndex == selectorIndex);

    auto item = epub->getTocItem(itemIndex);

    // Indent per TOC level while keeping content within the gutter-safe region.
    const int indentSize = contentX + 20 + (item.level - 1) * 15;
    const std::string chapterName =
        renderer.truncatedText(UI_10_FONT_ID, item.title.c_str(), contentWidth - 40 - indentSize);

    renderer.drawText(UI_10_FONT_ID, indentSize, displayY, chapterName.c_str(), !isSelected);
  }

  // Page count, in the strip every scrollable screen reserves for it. A TOC is
  // the longest list on the device -- the one place a silent scroll bar costs
  // the most.
  GUI.drawPageIndicator(renderer, listAreaRect(), selectorIndex / pageItems + 1,
                        (totalItems + pageItems - 1) / pageItems);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
