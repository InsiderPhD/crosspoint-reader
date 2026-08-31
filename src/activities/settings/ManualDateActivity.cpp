#include "ManualDateActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/HeaderDateUtils.h"
#include "util/TimeUtils.h"
#include "util/TouchListNav.h"

namespace {
constexpr int FIELD_COUNT = 3;
constexpr int MIN_DAY = 1;
constexpr int MAX_DAY = 31;
constexpr int MIN_MONTH = 1;
constexpr int MAX_MONTH = 12;
constexpr int MIN_YEAR = 2024;
constexpr int MAX_YEAR = 2099;

std::string formatTwoDigits(const unsigned value) {
  char buffer[4];
  snprintf(buffer, sizeof(buffer), "%02u", value);
  return buffer;
}

unsigned wrapValue(const unsigned value, const int delta, const unsigned minValue, const unsigned maxValue) {
  const int range = static_cast<int>(maxValue - minValue + 1);
  int offset = static_cast<int>(value - minValue) + delta;
  offset %= range;
  if (offset < 0) {
    offset += range;
  }
  return minValue + static_cast<unsigned>(offset);
}
}  // namespace

void ManualDateActivity::onEnter() {
  Activity::onEnter();
  TimeUtils::configureTimezone();

  year = 2026;
  month = 6;
  day = 15;

  const auto displayDateInfo = HeaderDateUtils::getDisplayDateInfo();
  const uint32_t referenceTimestamp = displayDateInfo.timestamp;

  if (TimeUtils::isClockValid(referenceTimestamp)) {
    time_t currentTime = static_cast<time_t>(referenceTimestamp);
    tm localTime = {};
    if (localtime_r(&currentTime, &localTime) != nullptr) {
      year = std::clamp(localTime.tm_year + 1900, MIN_YEAR, MAX_YEAR);
      month = static_cast<unsigned>(std::clamp(localTime.tm_mon + 1, MIN_MONTH, MAX_MONTH));
      day = static_cast<unsigned>(std::clamp(localTime.tm_mday, MIN_DAY, MAX_DAY));
    }
  }

  selectedField = 0;
  requestUpdate();
}

void ManualDateActivity::adjustSelectedField(const int delta) {
  if (selectedField == 0) {
    day = wrapValue(day, delta, MIN_DAY, MAX_DAY);
  } else if (selectedField == 1) {
    month = wrapValue(month, delta, MIN_MONTH, MAX_MONTH);
  } else {
    year = std::clamp(year + delta, MIN_YEAR, MAX_YEAR);
  }
  requestUpdate();
}

std::string ManualDateActivity::getSelectedDateLabel() const { return TimeUtils::formatDateParts(year, month, day); }

void ManualDateActivity::saveDate() {
  uint32_t epoch = 0;
  if (!TimeUtils::setCurrentDate(year, month, day, &epoch)) {
    return;
  }

  APP_STATE.registerValidTimeSync(epoch);
  APP_STATE.saveToFile();
  finish();
}

void ManualDateActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), FIELD_COUNT, selectedField,
                               /*hasSubtitle=*/true, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedField = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      // Tapping the field already under the cursor steps its value, matching
      // the Right button — the same two-tap spinner SessionDateEdit uses.
      // Committing is the action bar's Confirm; a second tap must not save, or
      // the only way to set a value by touch would be gone. (Row SELECTION is
      // the part that needed this: it is Up/Down, which on the X4 Pro exist
      // only as swipes, and Full Touch drops those. Left/Right are the physical
      // side keys, so stepping a value always had a hardware affordance.)
      adjustSelectedField(1);
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    saveDate();
    return;
  }

  buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this] {
    selectedField = ButtonNavigator::nextIndex(selectedField, FIELD_COUNT);
    requestUpdate();
  });

  buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this] {
    selectedField = ButtonNavigator::previousIndex(selectedField, FIELD_COUNT);
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustSelectedField(-1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustSelectedField(1); });
}

// The three date-field rows. Shared by render() and loop()'s tap hit-testing so
// the two can never disagree.
Rect ManualDateActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  return Rect{0, contentTop, renderer.getScreenWidth(), metrics.listWithSubtitleRowHeight * FIELD_COUNT};
}

void ManualDateActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const int sidePadding = metrics.contentSidePadding;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SET_DATE),
                 getSelectedDateLabel().c_str());

  const Rect fieldsRect = listRect();
  const int contentTop = fieldsRect.y;
  const int listHeight = fieldsRect.height;
  GUI.drawList(
      renderer, fieldsRect, FIELD_COUNT, selectedField,
      [](int index) {
        if (index == 0) return std::string(tr(STR_DAY));
        if (index == 1) return std::string(tr(STR_MONTH));
        return std::string(tr(STR_YEAR));
      },
      [this](int index) {
        if (index == 0) return formatTwoDigits(day);
        if (index == 1) return formatTwoDigits(month);
        return std::to_string(year);
      },
      nullptr, nullptr, false);

  const int hintTop = contentTop + listHeight + metrics.verticalSpacing;
  const int hintWidth = pageWidth - sidePadding * 2;
  const std::string hint = renderer.truncatedText(SMALL_FONT_ID, tr(STR_SET_DATE_HINT), hintWidth);
  renderer.drawText(SMALL_FONT_ID, sidePadding, hintTop, hint.c_str());

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, /*allSlots=*/true);

  renderer.displayBuffer();
}
