#include "TimeZoneSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TimeUtils.h"
#include "util/TimeZoneRegistry.h"
#include "util/TouchListNav.h"

void TimeZoneSelectActivity::onEnter() {
  Activity::onEnter();
  // A migrated quarter-hour offset with no table entry (e.g. UTC+0:15) has no
  // row to land on, so the cursor starts at the top of the list.
  const uint8_t current = TimeUtils::getTimeZoneIndex();
  selectedIndex = current < TimeZoneRegistry::getPresetCount() ? current : 0;
  requestUpdate();
}

void TimeZoneSelectActivity::handleSelection() {
  {
    RenderLock lock(*this);
    TimeUtils::setTimeZone(static_cast<uint8_t>(selectedIndex));
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
      selectedIndex = tappedIndex;
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
  // pageItems comes from the theme's own windowing of listRect(), so a page
  // swipe always moves by exactly the rows drawList drew.
  const int pageItems = GUI.listGeometry(listRect(), 0, /*hasSubtitle=*/false).pageItems;
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
  const int currentIndex = TimeUtils::getTimeZoneIndex();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TIME_ZONE));

  // Value column: the standard UTC offset for the named zones (the fixed
  // "UTC+HH:MM" rows already say it), replaced by "Selected" on the active row.
  GUI.drawList(
      renderer, listRect(), totalItems, selectedIndex,
      [](int index) { return std::string(TimeZoneRegistry::getPreset(static_cast<uint8_t>(index)).label); }, nullptr,
      nullptr,
      [currentIndex](int index) {
        if (index == currentIndex) return std::string(tr(STR_SELECTED));
        const TimeZonePreset& zone = TimeZoneRegistry::getPreset(static_cast<uint8_t>(index));
        if (strncmp(zone.label, "UTC", 3) == 0) return std::string();
        char offset[TimeZoneRegistry::OFFSET_BUFFER_SIZE];
        TimeZoneRegistry::formatOffset(zone.stdOffsetQ, offset, sizeof(offset));
        return std::string(offset);
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
