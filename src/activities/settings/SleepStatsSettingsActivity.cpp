#include "SleepStatsSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/boot_sleep/SleepStatsCard.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

namespace {
// The three sleep-stat picker slots, in list order.
constexpr int SLOT_COUNT = 3;

// Display name for each SLEEP_STAT value. Indexed by the enum (see
// CrossPointSettings::SLEEP_STAT); order must match it and SettingsList.h.
const StrId statNames[CrossPointSettings::SLEEP_STAT_COUNT] = {
    StrId::STR_NONE_OPT,           StrId::STR_STAT_TODAY,     StrId::STR_STAT_GOAL,
    StrId::STR_STAT_WEEK,          StrId::STR_STAT_STREAK,    StrId::STR_STAT_MONTH,
    StrId::STR_STAT_TOTAL,         StrId::STR_STAT_FINISHED,  StrId::STR_STAT_RETURN,
    StrId::STR_STAT_BOOK_PROGRESS, StrId::STR_STAT_DAILY_AVG, StrId::STR_STAT_DAYS_MONTH,
    StrId::STR_STAT_WEEK_STREAK,   StrId::STR_STAT_GOAL_LEFT, StrId::STR_STAT_BOOK_TIME_LEFT,
};

const StrId slotLabels[SLOT_COUNT] = {StrId::STR_SLEEP_STAT_1, StrId::STR_SLEEP_STAT_2, StrId::STR_SLEEP_STAT_3};

// Slot value accessors so navigation/preview stay index-driven.
uint8_t& slotRef(int index) {
  switch (index) {
    case 0:
      return SETTINGS.sleepStatSlot1;
    case 1:
      return SETTINGS.sleepStatSlot2;
    default:
      return SETTINGS.sleepStatSlot3;
  }
}
}  // namespace

void SleepStatsSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  changed = false;

  // Clamp any corrupt/migrated slot values so the value lookup stays in range.
  for (int i = 0; i < SLOT_COUNT; i++) {
    if (slotRef(i) >= CrossPointSettings::SLEEP_STAT_COUNT) {
      slotRef(i) = CrossPointSettings::SLEEP_STAT_NONE;
    }
  }

  requestUpdate();
}

void SleepStatsSettingsActivity::onExit() {
  if (changed) SETTINGS.saveToFile();
  Activity::onExit();
}

void SleepStatsSettingsActivity::handleSelection() {
  uint8_t& slot = slotRef(selectedIndex);
  slot = (slot + 1) % CrossPointSettings::SLEEP_STAT_COUNT;
  changed = true;
}

void SleepStatsSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), SLOT_COUNT, selectedIndex,
                               /*hasSubtitle=*/false, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedIndex = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      handleSelection();
      requestUpdate();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, SLOT_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, SLOT_COUNT);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, SLOT_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, SLOT_COUNT);
    requestUpdate();
  });
}

// List body between the header and the preview zone (the live card claims the
// lower part of the screen). Shared by render() and the loop()'s tap
// hit-testing so the two can never disagree.
Rect SleepStatsSettingsActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Reserve the lower part of the screen for the live preview card; the short
  // 3-row list sits above it. The card renders itself centred and bottom-anchored.
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int previewZoneHeight = lineH * SleepStatsCard::MAX_STAT_LINES + 70;
  const int cardBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int previewZoneTop = cardBottom - previewZoneHeight;

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = previewZoneTop - contentTop - metrics.verticalSpacing;
  return Rect{0, contentTop, renderer.getScreenWidth(), listHeight};
}

void SleepStatsSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SLEEP_SCREEN_STATS));

  const Rect list = listRect();
  // Derived from the list rect so the preview zone tracks it.
  const int previewZoneTop = list.y + list.height + metrics.verticalSpacing;
  const int cardBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

  GUI.drawList(
      renderer, list, SLOT_COUNT, selectedIndex, [](int index) { return std::string(I18N.get(slotLabels[index])); },
      nullptr, nullptr, [](int index) -> std::string { return std::string(I18N.get(statNames[slotRef(index)])); },
      true);

  // "Preview" label above the card, then the live card using real stats with
  // sample fallbacks so every selected slot demonstrates its format.
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, previewZoneTop, tr(STR_PREVIEW));
  SleepStatsCard::draw(renderer, /*clearRegionOnly=*/false, cardBottom, /*previewMode=*/true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CHANGE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
