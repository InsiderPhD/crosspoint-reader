#include "FrontlightScheduleEditActivity.h"

#if FREEINK_CAP_FRONTLIGHT

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <memory>
#include <string>

#include "FrontlightBrightnessActivity.h"
#include "MappedInputManager.h"
#include "activities/util/ListPickerActivity.h"
#include "components/UITheme.h"
#include "util/FrontlightScheduler.h"
#include "util/FrontlightToggle.h"
#include "util/TouchListNav.h"

namespace {
enum Field : int { FIELD_ENABLED = 0, FIELD_START, FIELD_END, FIELD_BRIGHTNESS, FIELD_WARMTH, FIELD_COUNT };

// Windows are set on the hour: 24 rows is one short scroll, and a bedtime
// light does not need finer than that.
constexpr int HOURS_PER_DAY = 24;
constexpr int WARMTH_STEP = 10;
constexpr int WARMTH_STEPS = 100 / WARMTH_STEP + 1;  // 0, 10, ... 100

std::string hourLabel(const int hour) {
  char buf[10];
  FrontlightScheduler::formatMinuteOfDay(static_cast<uint16_t>(hour * 60), buf, sizeof(buf));
  return buf;
}

std::string percentLabel(const uint8_t percent) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%u%%", percent);
  return buf;
}

const StrId kSlotLabels[CrossPointSettings::FRONTLIGHT_SCHEDULE_SLOTS] = {
    StrId::STR_FRONTLIGHT_SCHEDULE_1, StrId::STR_FRONTLIGHT_SCHEDULE_2, StrId::STR_FRONTLIGHT_SCHEDULE_3,
    StrId::STR_FRONTLIGHT_SCHEDULE_4};
}  // namespace

void FrontlightScheduleEditActivity::onEnter() {
  Activity::onEnter();
  draft = SETTINGS.frontlightSchedules[slot];
  // Hour-granular: a stored value off the hour (older file, web edit) is shown
  // and saved as its hour.
  draft.startMinutes = static_cast<uint16_t>((draft.startMinutes / 60) * 60);
  draft.endMinutes = static_cast<uint16_t>((draft.endMinutes / 60) * 60);
  selectedField = 0;
  previewDraft();
  requestUpdate();
}

// Single-channel boards have no warmth row.
int FrontlightScheduleEditActivity::fieldCount() const {
  return halFrontlight.hasWarmth() ? FIELD_COUNT : FIELD_WARMTH;
}

// Light the panel as this schedule would. A schedule whose level is Off has
// nothing to show, so its warmth is previewed at the dim fallback instead.
void FrontlightScheduleEditActivity::previewDraft() {
  halFrontlight.apply(draft.brightness > 0 ? draft.brightness : FrontlightToggle::FALLBACK_BRIGHTNESS, draft.warmth);
}

// Leaving the editor: put the real setting back.
void FrontlightScheduleEditActivity::restoreLight() {
  halFrontlight.apply(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth);
}

void FrontlightScheduleEditActivity::pickHour(const bool start) {
  const uint16_t current = start ? draft.startMinutes : draft.endMinutes;
  startActivityForResult(
      std::make_unique<ListPickerActivity>(renderer, mappedInput, start ? tr(STR_SCHEDULE_START) : tr(STR_SCHEDULE_END),
                                           HOURS_PER_DAY, current / 60, hourLabel),
      [this, start](const ActivityResult& result) {
        const auto* pick = std::get_if<ListPickResult>(&result.data);
        if (!pick) return;  // cancelled
        const auto minutes = static_cast<uint16_t>(pick->index * 60);
        if (start) {
          draft.startMinutes = minutes;
        } else {
          draft.endMinutes = minutes;
        }
      });
}

void FrontlightScheduleEditActivity::pickBrightness() {
  startActivityForResult(
      std::make_unique<FrontlightBrightnessActivity>(renderer, mappedInput, draft.brightness, draft.warmth),
      [this](const ActivityResult& result) {
        if (const auto* picked = std::get_if<FrontlightResult>(&result.data)) {
          draft.brightness = picked->brightness;
        }
        // Also covers the cancel path, where the picker put the old draft level back.
        previewDraft();
      });
}

void FrontlightScheduleEditActivity::pickWarmth() {
  // Previewed at the schedule's own brightness so the mix is judged at the
  // level it will actually be lit at; a schedule whose level is Off borrows
  // the dim fallback so there is something to see.
  const uint8_t previewBrightness = draft.brightness > 0 ? draft.brightness : FrontlightToggle::FALLBACK_BRIGHTNESS;
  startActivityForResult(std::make_unique<ListPickerActivity>(
                             renderer, mappedInput, tr(STR_FRONTLIGHT_WARMTH), WARMTH_STEPS, draft.warmth / WARMTH_STEP,
                             [](int index) { return percentLabel(static_cast<uint8_t>(index * WARMTH_STEP)); },
                             [previewBrightness](int index) {
                               halFrontlight.apply(previewBrightness, static_cast<uint8_t>(index * WARMTH_STEP));
                             }),
                         [this](const ActivityResult& result) {
                           if (const auto* pick = std::get_if<ListPickResult>(&result.data)) {
                             draft.warmth = static_cast<uint8_t>(pick->index * WARMTH_STEP);
                           }
                           previewDraft();
                         });
}

void FrontlightScheduleEditActivity::activateField(const int index) {
  switch (index) {
    case FIELD_ENABLED:
      draft.enabled = draft.enabled ? 0 : 1;
      requestUpdate();
      break;
    case FIELD_START:
      pickHour(true);
      break;
    case FIELD_END:
      pickHour(false);
      break;
    case FIELD_BRIGHTNESS:
      pickBrightness();
      break;
    case FIELD_WARMTH:
      pickWarmth();
      break;
    default:
      break;
  }
}

void FrontlightScheduleEditActivity::saveAndExit() {
  restoreLight();
  SETTINGS.frontlightSchedules[slot] = draft;
  setResult(ActivityResult{});
  finish();
}

void FrontlightScheduleEditActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    saveAndExit();
    return;
  }

  const int count = fieldCount();
  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), count, selectedField,
                               /*hasSubtitle=*/false, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedField = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      selectedField = tappedIndex;
      activateField(selectedField);
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateField(selectedField);
    return;
  }

  buttonNavigator.onNextRelease([this, count] {
    selectedField = ButtonNavigator::nextIndex(selectedField, count);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, count] {
    selectedField = ButtonNavigator::previousIndex(selectedField, count);
    requestUpdate();
  });
}

// The field rows. Shared by render() and loop()'s tap hit-testing so the two
// can never disagree.
Rect FrontlightScheduleEditActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  // Covers the fields AND the page-counter strip every list rect gives up at
  // its bottom — without it the strip would eat a field.
  return Rect{0, contentTop, renderer.getScreenWidth(), GUI.listRectHeightForRows(fieldCount(), /*hasSubtitle=*/false)};
}

std::string FrontlightScheduleEditActivity::fieldLabel(const int index) const {
  switch (index) {
    case FIELD_ENABLED:
      return tr(STR_SCHEDULE_ENABLED);
    case FIELD_START:
      return tr(STR_SCHEDULE_START);
    case FIELD_END:
      return tr(STR_SCHEDULE_END);
    case FIELD_BRIGHTNESS:
      return tr(STR_FRONTLIGHT_BRIGHTNESS);
    case FIELD_WARMTH:
      return tr(STR_FRONTLIGHT_WARMTH);
    default:
      return "";
  }
}

std::string FrontlightScheduleEditActivity::fieldValue(const int index) const {
  switch (index) {
    case FIELD_ENABLED:
      return draft.enabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case FIELD_START:
      return hourLabel(draft.startMinutes / 60);
    case FIELD_END:
      return hourLabel(draft.endMinutes / 60);
    case FIELD_BRIGHTNESS:
      return draft.brightness == 0 ? std::string(tr(STR_STATE_OFF)) : percentLabel(draft.brightness);
    case FIELD_WARMTH:
      return percentLabel(draft.warmth);
    default:
      return "";
  }
}

void FrontlightScheduleEditActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FRONTLIGHT_SCHEDULE),
                 I18N.get(kSlotLabels[slot]));

  GUI.drawList(
      renderer, listRect(), fieldCount(), selectedField, [this](int index) { return fieldLabel(index); }, nullptr,
      nullptr, [this](int index) { return fieldValue(index); }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_SAVE_AND_BACK), tr(STR_CHANGE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}

#endif  // FREEINK_CAP_FRONTLIGHT
