#include "FrontlightBrightnessActivity.h"

#if FREEINK_CAP_FRONTLIGHT

#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "util/FrontlightLevels.h"
#include "util/TouchListNav.h"

namespace {
// "Off" or "40%". Rows are short, so a stack buffer keeps this off the heap.
std::string levelLabel(const int index) {
  const uint8_t percent = FrontlightLevels::LEVELS[index];
  if (percent == 0) {
    return tr(STR_STATE_OFF);
  }
  char buf[8];
  snprintf(buf, sizeof(buf), "%u%%", percent);
  return std::string(buf);
}
}  // namespace

void FrontlightBrightnessActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = FrontlightLevels::nearestIndex(startBrightness);
  requestUpdate();
}

// Every cursor move repaints the list anyway, so driving the light here costs
// nothing extra and is the whole point of a picker over a value row.
void FrontlightBrightnessActivity::moveTo(const int index) {
  selectedIndex = index;
  halFrontlight.apply(FrontlightLevels::LEVELS[selectedIndex], warmth);
  requestUpdate();
}

void FrontlightBrightnessActivity::confirmSelection() {
  setResult(FrontlightResult{FrontlightLevels::LEVELS[selectedIndex]});
  finish();
}

void FrontlightBrightnessActivity::cancel() {
  halFrontlight.apply(startBrightness, warmth);
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void FrontlightBrightnessActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), FrontlightLevels::COUNT, selectedIndex,
                               /*hasSubtitle=*/false, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      moveTo(tappedIndex);
      return;
    case TouchListNav::TapResult::Activated:
      confirmSelection();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmSelection();
    return;
  }

  buttonNavigator.onNextRelease([this] { moveTo(ButtonNavigator::nextIndex(selectedIndex, FrontlightLevels::COUNT)); });
  buttonNavigator.onPreviousRelease(
      [this] { moveTo(ButtonNavigator::previousIndex(selectedIndex, FrontlightLevels::COUNT)); });

  // Full Touch: a vertical swipe turns a page, matching the held side key.
  // pageItems mirrors drawList's windowing of listRect() (rect.height / rowHeight).
  const int pageItems = listRect().height / UITheme::getInstance().getMetrics().listRowHeight;
  int pagedIndex = selectedIndex;
  if (TouchListNav::pageSwipe(mappedInput, FrontlightLevels::COUNT, pageItems, pagedIndex)) {
    moveTo(pagedIndex);
    return;
  }
}

// List body between the header and the button hints. Shared by render() and
// the loop()'s tap hit-testing so the two can never disagree.
Rect FrontlightBrightnessActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void FrontlightBrightnessActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FRONTLIGHT_BRIGHTNESS));

  // The cursor is already the live level, so the right-hand marker flags where
  // the user started — the level they fall back to if they back out.
  const int startIndex = FrontlightLevels::nearestIndex(startBrightness);
  GUI.drawList(
      renderer, listRect(), FrontlightLevels::COUNT, selectedIndex, [](int index) { return levelLabel(index); },
      nullptr, nullptr, [startIndex](int index) { return index == startIndex ? tr(STR_SELECTED) : ""; }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}

#endif  // FREEINK_CAP_FRONTLIGHT
