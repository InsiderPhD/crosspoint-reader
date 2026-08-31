#include "EpubReaderFootnotesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

void EpubReaderFootnotesActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void EpubReaderFootnotesActivity::onExit() { Activity::onExit(); }

// Top edge of the first visible row. Mirrors render()'s
// fillRect(0, 60 + contentY + row * lineHeight, ..., lineHeight).
// Keep in sync with render() or taps land on the wrong row.
int EpubReaderFootnotesActivity::listTopY() const {
  const bool isPortraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int contentY = isPortraitInverted ? 50 : 0;
  return 60 + contentY;
}

// Size of the scroll window. Mirrors render()'s visibleCount verbatim.
int EpubReaderFootnotesActivity::visibleRows() const {
  const bool isPortraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int contentY = isPortraitInverted ? 50 : 0;
  return std::max(1, (renderer.getScreenHeight() - contentY) / ROW_H);
}

// The Confirm short-press body, also fired by a Full Touch tap on the selected row.
void EpubReaderFootnotesActivity::activateSelectedFootnote() {
  if (selectedIndex >= 0 && selectedIndex < static_cast<int>(footnotes.size())) {
    setResult(FootnoteResult{footnotes[selectedIndex].href});
    finish();
  }
}

void EpubReaderFootnotesActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

#if FREEINK_DEVICE_X4PRO
  // Full Touch: first tap on a row moves the cursor there; a second tap on the
  // already-selected row follows the footnote. The list is a scroll window, so
  // the tapped item is scrollOffset (as of the last render) plus the row.
  if (SETTINGS.fullTouchUi && !footnotes.empty()) {
    int lx, ly;
    if (mappedInput.wasTapPoint(lx, ly)) {
      const int top = listTopY();
      const int row = (ly - top) / ROW_H;
      if (ly >= top && row >= 0 && row < visibleRows()) {
        const int itemIndex = scrollOffset + row;
        if (itemIndex < static_cast<int>(footnotes.size())) {
          if (itemIndex != selectedIndex) {
            selectedIndex = itemIndex;
            requestUpdate();
          } else {
            activateSelectedFootnote();
          }
          return;
        }
      }
      // Dead space (title bar, below the last row): no action.
    }
  }
#endif

  // Full Touch: the footnote list is a scroll window, so a vertical swipe jumps
  // a whole page of it — same content-drag sense as every other list. Inert
  // unless it actually scrolls (pageSwipe's own guard).
  if (TouchListNav::pageSwipe(mappedInput, static_cast<int>(footnotes.size()), visibleRows(), selectedIndex)) {
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelectedFootnote();
    return;
  }

  buttonNavigator.onNext([this] {
    if (!footnotes.empty()) {
      selectedIndex = (selectedIndex + 1) % footnotes.size();
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (!footnotes.empty()) {
      selectedIndex = (selectedIndex - 1 + footnotes.size()) % footnotes.size();
      requestUpdate();
    }
  });
}

void EpubReaderFootnotesActivity::render(RenderLock&&) {
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

  // Manual centering to honor content gutters.
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_FOOTNOTES), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_FOOTNOTES), true, EpdFontFamily::BOLD);

  if (footnotes.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 90 + contentY, tr(STR_NO_FOOTNOTES));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  constexpr int lineHeight = ROW_H;
  const int screenWidth = renderer.getScreenWidth();
  const int marginLeft = contentX + 20;

  // listTopY()/visibleRows() mirror the values below and are shared with the tap
  // hit-testing in loop().
  const int listTop = listTopY();
  const int visibleCount = visibleRows();
  if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
  if (selectedIndex >= scrollOffset + visibleCount) scrollOffset = selectedIndex - visibleCount + 1;

  for (int i = scrollOffset; i < static_cast<int>(footnotes.size()) && i < scrollOffset + visibleCount; i++) {
    const int y = listTop + (i - scrollOffset) * lineHeight;
    const bool isSelected = (i == selectedIndex);

    if (isSelected) {
      renderer.fillRect(0, y, screenWidth, lineHeight, true);
    }

    // Show footnote number + body text if available, otherwise fall back to number only
    const auto& fn = footnotes[i];
    char buf[160];
    if (fn.hasText()) {
      snprintf(buf, sizeof(buf), "%s %s", fn.number[0] != '\0' ? fn.number : tr(STR_LINK), fn.text.get());
    } else {
      snprintf(buf, sizeof(buf), "%s", fn.number[0] != '\0' ? fn.number : tr(STR_LINK));
    }
    renderer.drawText(UI_10_FONT_ID, marginLeft, y + 4, buf, !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
