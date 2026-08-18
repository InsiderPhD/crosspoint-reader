#include "TimeZoneSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TimeUtils.h"
#include "util/TimeZoneRegistry.h"
#include "util/TouchListNav.h"

void TimeZoneSelectActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = TimeZoneRegistry::clampPresetIndex(SETTINGS.timeZonePreset);
  requestUpdate();
}

void TimeZoneSelectActivity::handleSelection() {
  {
    RenderLock lock(*this);
    SETTINGS.timeZonePreset = TimeZoneRegistry::clampPresetIndex(static_cast<uint8_t>(selectedIndex));
    SETTINGS.saveToFile();
    TimeUtils::configureTimezone();
  }
  finish();
}

void TimeZoneSelectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), static_cast<int>(TimeZoneRegistry::getPresetCount()),
                               selectedIndex, /*hasSubtitle=*/false, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedIndex = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      handleSelection();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int totalItems = static_cast<int>(TimeZoneRegistry::getPresetCount());
  buttonNavigator.onNextRelease([this, totalItems] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, totalItems] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
    requestUpdate();
  });

  // Full Touch: a vertical swipe turns a page, matching the held side key.
  // pageItems mirrors drawList's windowing of listRect() (rect.height / rowHeight).
  const int pageItems = listRect().height / UITheme::getInstance().getMetrics().listRowHeight;
  if (TouchListNav::pageSwipe(mappedInput, totalItems, pageItems, selectedIndex)) {
    requestUpdate();
    return;
  }

  buttonNavigator.onNextContinuous([this, totalItems] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, totalItems] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
    requestUpdate();
  });
}

// List body between the header and the button hints. Shared by render() and
// the loop()'s tap hit-testing so the two can never disagree.
Rect TimeZoneSelectActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void TimeZoneSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const int totalItems = static_cast<int>(TimeZoneRegistry::getPresetCount());
  const int currentIndex = TimeZoneRegistry::clampPresetIndex(SETTINGS.timeZonePreset);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TIME_ZONE));

  GUI.drawList(
      renderer, listRect(), totalItems, selectedIndex,
      [](int index) { return std::string(TimeZoneRegistry::getPresetLabel(static_cast<uint8_t>(index))); }, nullptr,
      nullptr,
      [currentIndex](int index) { return index == currentIndex ? std::string(tr(STR_SELECTED)) : std::string(""); },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
