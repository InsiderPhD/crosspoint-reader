#include "SessionDateEditActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/HeaderDateUtils.h"
#include "util/TimeUtils.h"

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

void SessionDateEditActivity::onEnter() {
  Activity::onEnter();
  TimeUtils::configureTimezone();

  // Seed from the session's existing date if there is one. For sessions with
  // dayOrdinal=0 (no date yet) we fall back to the header's display date so
  // the user lands on "today" rather than 2026-06-15.
  const auto& log = READING_STATS.getSessionLog();
  uint32_t seedDayOrdinal = 0;
  if (sessionIndex < log.size()) {
    seedDayOrdinal = log[sessionIndex].dayOrdinal;
  }
  if (seedDayOrdinal == 0) {
    const auto displayDateInfo = HeaderDateUtils::getDisplayDateInfo();
    if (TimeUtils::isClockValid(displayDateInfo.timestamp)) {
      seedDayOrdinal = TimeUtils::getLocalDayOrdinal(displayDateInfo.timestamp);
    }
  }

  year = 2026;
  month = 6;
  day = 15;
  if (seedDayOrdinal != 0) {
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    if (TimeUtils::getDateFromDayOrdinal(seedDayOrdinal, y, m, d)) {
      year = std::clamp(y, MIN_YEAR, MAX_YEAR);
      month = std::clamp<unsigned>(m, MIN_MONTH, MAX_MONTH);
      day = std::clamp<unsigned>(d, MIN_DAY, MAX_DAY);
    }
  }

  selectedField = 0;
  requestUpdate();
}

void SessionDateEditActivity::adjustSelectedField(const int delta) {
  if (selectedField == 0) {
    day = wrapValue(day, delta, MIN_DAY, MAX_DAY);
  } else if (selectedField == 1) {
    month = wrapValue(month, delta, MIN_MONTH, MAX_MONTH);
  } else {
    year = std::clamp(year + delta, MIN_YEAR, MAX_YEAR);
  }
  requestUpdate();
}

std::string SessionDateEditActivity::getSelectedDateLabel() const {
  return TimeUtils::formatDateParts(year, month, day);
}

void SessionDateEditActivity::saveDate() {
  const uint32_t dayOrdinal = TimeUtils::getDayOrdinalForDate(year, month, static_cast<unsigned>(day));
  // editSessionDate returns false for invalid index, dayOrdinal=0, no bookId,
  // or no actual change — in any of those cases we still exit; the user has
  // no other action available from this screen.
  READING_STATS.editSessionDate(sessionIndex, dayOrdinal);
  finish();
}

void SessionDateEditActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
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

void SessionDateEditActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const int sidePadding = metrics.contentSidePadding;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SET_DATE),
                 getSelectedDateLabel().c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = metrics.listWithSubtitleRowHeight * FIELD_COUNT;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, listHeight}, FIELD_COUNT, selectedField,
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
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
