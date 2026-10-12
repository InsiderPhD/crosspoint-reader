#include "EpubReaderPercentSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Fine/coarse slider step sizes for percent adjustments.
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;

// Popup card geometry. The card is sized from the fonts and capped to the
// screen, so it fits all four orientations on every panel.
constexpr int kCardMaxWidth = 420;
constexpr int kCardSideMargin = 20;  // minimum gap between card and screen edge
constexpr int kCardPadding = 16;
constexpr int kCardFrame = 2;  // same frame thickness as GUI.drawPopup
constexpr int kRowGap = 12;
constexpr int kBarHeight = 16;
constexpr int kKnobWidth = 4;
constexpr int kKnobOverhang = 4;
}  // namespace

void EpubReaderPercentSelectionActivity::onEnter() {
  Activity::onEnter();
  // Set up rendering task and mark first frame dirty.
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::onExit() { Activity::onExit(); }

void EpubReaderPercentSelectionActivity::adjustPercent(const int delta) {
  // Apply delta and clamp within 0-100.
  percent += delta;
  if (percent < 0) {
    percent = 0;
  } else if (percent > 100) {
    percent = 100;
  }
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::loop() {
  // Back cancels, confirm selects, arrows adjust the percent.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(PercentResult{percent});
    finish();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustPercent(-kSmallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustPercent(kSmallStep); });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { adjustPercent(kLargeStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { adjustPercent(-kLargeStep); });
}

void EpubReaderPercentSelectionActivity::render(RenderLock&&) {
  // A popup over the screen it was opened from (upstream #3669): no
  // clearScreen(). The single framebuffer still holds that screen's last frame,
  // and nothing else draws while this activity is on top, so the card is simply
  // repainted in place on every step. There is deliberately no page snapshot
  // here — upstream restores the clean page from a stored copy, which on this
  // board would be a second 48KB block held while a book is open.
  //
  // Dark mode: every screen pushes its frame inverted and leaves the buffer
  // that way, this one included. Flip back to logical polarity before drawing
  // so the card and the frame under it invert together at the end, the same
  // "invert, draw, invert" the reader's own popups use.
  if (SETTINGS.darkMode) renderer.invertScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  if (CrossPointSettings::FULL_TOUCH_UI) {
    // The tap-target strip is laid out in this logical frame and its slots
    // depend on how many labels a screen shows, so wipe the previous screen's
    // strip before drawing ours. Button boards redraw the same four opaque
    // boxes in the same places and need no wipe.
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

  // Title and numeric percent value. The card is horizontally centred, so
  // screen-centred text is card-centred too.
  int y = cardY + kCardPadding;
  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_GO_TO_PERCENT), true, EpdFontFamily::BOLD);
  y += titleLineHeight + kRowGap;

  char percentText[8];
  snprintf(percentText, sizeof(percentText), "%d%%", percent);
  renderer.drawCenteredText(UI_12_FONT_ID, y, percentText, true, EpdFontFamily::BOLD);
  y += titleLineHeight + kRowGap + kKnobOverhang;

  // Slider track, fill and knob.
  const int barWidth = cardWidth - kCardPadding * 2;
  const int barX = cardX + kCardPadding;
  renderer.drawRect(barX, y, barWidth, kBarHeight);
  const int fillWidth = (barWidth - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, y + 2, fillWidth, kBarHeight - 4);
  }
  const int knobX = barX + 2 + fillWidth - kKnobWidth / 2;
  renderer.fillRect(knobX, y - kKnobOverhang, kKnobWidth, kBarHeight + kKnobOverhang * 2, true);
  y += kBarHeight + kKnobOverhang + kRowGap;

  // Hint text for step sizes.
  renderer.drawCenteredText(SMALL_FONT_ID, y, tr(STR_PERCENT_STEP_HINT), true);

  // Button hints follow the current front button layout.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, /*allSlots=*/true);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
