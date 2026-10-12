#include "EpubReaderPageSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;

// Popup card geometry, the same card as Go to % so the two read as one family.
constexpr int kCardMaxWidth = 420;
constexpr int kCardSideMargin = 20;  // minimum gap between card and screen edge
constexpr int kCardPadding = 16;
constexpr int kCardFrame = 2;  // same frame thickness as GUI.drawPopup
constexpr int kRowGap = 12;
constexpr int kBarHeight = 16;
constexpr int kKnobWidth = 4;
constexpr int kKnobOverhang = 4;
}  // namespace

void EpubReaderPageSelectionActivity::onEnter() {
  Activity::onEnter();
  loadLabel();
  requestUpdate();
}

void EpubReaderPageSelectionActivity::onExit() { Activity::onExit(); }

void EpubReaderPageSelectionActivity::loadLabel() {
  PageList::Entry entry;
  if (PageList::read(cachePath, index, entry)) {
    snprintf(label, sizeof(label), "%s", entry.label);
  } else {
    // Unreadable record: still show where in the list the selection is.
    snprintf(label, sizeof(label), "%u", static_cast<unsigned>(index) + 1);
  }
}

void EpubReaderPageSelectionActivity::adjust(const int delta) {
  const int moved = std::clamp(static_cast<int>(index) + delta, 0, static_cast<int>(pageCount) - 1);
  if (moved == index) return;
  index = static_cast<uint16_t>(moved);
  loadLabel();
  requestUpdate();
}

void EpubReaderPageSelectionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(PageResult{index});
    finish();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjust(-kSmallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjust(kSmallStep); });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { adjust(kLargeStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { adjust(-kLargeStep); });
}

void EpubReaderPageSelectionActivity::render(RenderLock&&) {
  // A popup over the screen it was opened from: no clearScreen(), no snapshot
  // of the page underneath. See EpubReaderPercentSelectionActivity::render for
  // why, and for the dark-mode "invert, draw, invert" around it.
  if (SETTINGS.darkMode) renderer.invertScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  if (CrossPointSettings::FULL_TOUCH_UI) {
    // Wipe the previous screen's tap-target strip before drawing ours.
    renderer.fillRect(0, screenHeight - metrics.buttonHintsHeight, screenWidth, metrics.buttonHintsHeight, false);
  }

  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int hintLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int cardWidth = std::min(kCardMaxWidth, screenWidth - kCardSideMargin * 2);
  const int cardHeight =
      kCardPadding * 2 + titleLineHeight * 2 + kRowGap * 3 + kBarHeight + kKnobOverhang * 2 + hintLineHeight;
  const int cardX = (screenWidth - cardWidth) / 2;
  // Centred in the space above the button hints; never above the screen top.
  const int cardY = std::max(kCardFrame, (screenHeight - metrics.buttonHintsHeight - cardHeight) / 2);

  renderer.fillRect(cardX - kCardFrame, cardY - kCardFrame, cardWidth + kCardFrame * 2, cardHeight + kCardFrame * 2,
                    true);
  renderer.fillRect(cardX, cardY, cardWidth, cardHeight, false);

  int y = cardY + kCardPadding;
  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_GO_TO_PAGE), true, EpdFontFamily::BOLD);
  y += titleLineHeight + kRowGap;

  renderer.drawCenteredText(UI_12_FONT_ID, y, label, true, EpdFontFamily::BOLD);
  y += titleLineHeight + kRowGap + kKnobOverhang;

  // Slider track, fill and knob: position within the page list.
  const int barWidth = cardWidth - kCardPadding * 2;
  const int barX = cardX + kCardPadding;
  renderer.drawRect(barX, y, barWidth, kBarHeight);
  const int fillWidth = pageCount > 1 ? (barWidth - 4) * index / (pageCount - 1) : 0;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, y + 2, fillWidth, kBarHeight - 4);
  }
  const int knobX = barX + 2 + fillWidth - kKnobWidth / 2;
  renderer.fillRect(knobX, y - kKnobOverhang, kKnobWidth, kBarHeight + kKnobOverhang * 2, true);
  y += kBarHeight + kKnobOverhang + kRowGap;

  renderer.drawCenteredText(SMALL_FONT_ID, y, tr(STR_PAGE_STEP_HINT), true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, /*allSlots=*/true);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
