#include "FrontlightScheduleListActivity.h"

#if FREEINK_CAP_FRONTLIGHT

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <memory>

#include "CrossPointSettings.h"
#include "FrontlightScheduleEditActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "util/FrontlightScheduler.h"
#include "util/TimeUtils.h"
#include "util/TouchListNav.h"

namespace {
constexpr int kRowCount = CrossPointSettings::FRONTLIGHT_SCHEDULE_SLOTS;

const StrId kSlotLabels[kRowCount] = {StrId::STR_FRONTLIGHT_SCHEDULE_1, StrId::STR_FRONTLIGHT_SCHEDULE_2,
                                      StrId::STR_FRONTLIGHT_SCHEDULE_3, StrId::STR_FRONTLIGHT_SCHEDULE_4};

// "21:00 - 07:00 · 10% · Warmth 80%", or Off. Short enough for a stack buffer.
std::string describeSlot(const int index) {
  const auto& slot = SETTINGS.frontlightSchedules[index];
  if (!slot.enabled) return tr(STR_STATE_OFF);
  char start[10];
  char end[10];
  FrontlightScheduler::formatMinuteOfDay(slot.startMinutes, start, sizeof(start));
  FrontlightScheduler::formatMinuteOfDay(slot.endMinutes, end, sizeof(end));
  char buf[64];
  if (halFrontlight.hasWarmth()) {
    snprintf(buf, sizeof(buf), "%s - %s \xC2\xB7 %u%% \xC2\xB7 %s %u%%", start, end, slot.brightness, tr(STR_WARMTH),
             slot.warmth);
  } else {
    snprintf(buf, sizeof(buf), "%s - %s \xC2\xB7 %u%%", start, end, slot.brightness);
  }
  return buf;
}
}  // namespace

void FrontlightScheduleListActivity::onEnter() {
  Activity::onEnter();
  selectedRow = 0;
  isDirty = false;
  requestUpdate();
}

// A slot edited while its own window is lit would otherwise keep the old level
// until the next edge: the scheduler only acts on transitions.
void FrontlightScheduleListActivity::applySlotIfActive(const uint8_t slot) {
  if (SETTINGS.frontlightScheduleActive != slot + 1) return;
  const auto& sched = SETTINGS.frontlightSchedules[slot];
  if (!sched.enabled) return;  // evaluate() below restores the daytime level
  SETTINGS.frontlightBrightness = sched.brightness;
  SETTINGS.frontlightWarmth = sched.warmth;
  halFrontlight.apply(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth);
}

void FrontlightScheduleListActivity::editSlot(const uint8_t slot) {
  startActivityForResult(std::make_unique<FrontlightScheduleEditActivity>(renderer, mappedInput, slot),
                         [this, slot](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           // The editor wrote the slot; batch the save to this screen's exit.
                           isDirty = true;
                           applySlotIfActive(slot);
                           // Enter or leave a window immediately. Saves on its own when it
                           // moves the light, which covers the slot edit too.
                           if (FrontlightScheduler::evaluate()) isDirty = false;
                         });
}

void FrontlightScheduleListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (isDirty) {
      SETTINGS.saveToFile();
    }
    finish();
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), kRowCount, selectedRow,
                               /*hasSubtitle=*/true, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedRow = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      selectedRow = tappedIndex;
      editSlot(static_cast<uint8_t>(selectedRow));
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    editSlot(static_cast<uint8_t>(selectedRow));
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedRow = ButtonNavigator::nextIndex(selectedRow, kRowCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedRow = ButtonNavigator::previousIndex(selectedRow, kRowCount);
    requestUpdate();
  });
}

// The four slot rows. Shared by render() and loop()'s tap hit-testing so the
// two can never disagree.
Rect FrontlightScheduleListActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int topOffset = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  return Rect{0, topOffset, renderer.getScreenWidth(), GUI.listRectHeightForRows(kRowCount, /*hasSubtitle=*/true)};
}

void FrontlightScheduleListActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FRONTLIGHT_SCHEDULE));

  const Rect rows = listRect();
  GUI.drawList(
      renderer, rows, kRowCount, selectedRow, [](int index) { return std::string(I18N.get(kSlotLabels[index])); },
      [](int index) { return describeSlot(index); }, nullptr, nullptr, false);

  // Below the rows, not below the rect: the rect's bottom strip belongs to the
  // page counter (unused on a list this short).
  const int helpTop = rows.y + GUI.contentHeightWithoutIndicator(rows) + 2 * metrics.verticalSpacing;
  GUI.drawHelpText(renderer, Rect{0, helpTop, pageWidth, 20}, tr(STR_FRONTLIGHT_SCHEDULE_HINT));
  // Said up front rather than discovered as a schedule that never fires: with
  // no trustworthy clock the scheduler idles until NTP sets one.
  uint16_t minuteOfDay;
  if (!TimeUtils::getLocalMinuteOfDay(minuteOfDay)) {
    GUI.drawHelpText(renderer, Rect{0, helpTop + 20 + metrics.verticalSpacing, pageWidth, 20},
                     tr(STR_FRONTLIGHT_SCHEDULE_NO_CLOCK));
  }

  const auto hints = mappedInput.mapLabels(tr(STR_SAVE_AND_BACK), tr(STR_CHANGE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}

#endif  // FREEINK_CAP_FRONTLIGHT
