#include "ReaderControlsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Row layout:
//  0  Back      Short press  → readerShortPressBack
//  1  Back      Long press   → readerLongPressBack
//  2  Confirm   Short press  → readerShortPressConfirm
//  3  Confirm   Long press   → readerLongPressConfirm
//  4  Left      Short press  → readerShortPressLeft
//  5  Left      Long press   → readerLongPressLeft
//  6  Right     Short press  → readerShortPressRight
//  7  Right     Long press   → readerLongPressRight
//  8  Side Up   Short press  → readerShortPressSideUp
//  9  Side Up   Long press   → readerLongPressSideUp
// 10  Side Down Short press  → readerShortPressSideDown
// 11  Side Down Long press   → readerLongPressSideDown
// 12  Power     Short press  → readerShortPressPower
// 13  Power     Long press   → (fixed: Sleep)

constexpr uint8_t kFixedRow = 13;
}  // namespace

void ReaderControlsActivity::onEnter() {
  Activity::onEnter();
  selectedRow = 0;
  isDirty = false;
  requestUpdate();
}

void ReaderControlsActivity::onExit() { Activity::onExit(); }

void ReaderControlsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (isDirty) {
      SETTINGS.saveToFile();
    }
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    cycleActionForRow(selectedRow);
    isDirty = true;
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedRow = static_cast<uint8_t>(ButtonNavigator::nextIndex(selectedRow, kTotalRows));
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedRow = static_cast<uint8_t>(ButtonNavigator::previousIndex(selectedRow, kTotalRows));
    requestUpdate();
  });
}

void ReaderControlsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_READER_CONTROLS));

  const int topOffset = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - topOffset - metrics.buttonHintsHeight - metrics.verticalSpacing;

  GUI.drawList(renderer, Rect{0, topOffset, pageWidth, contentHeight}, kTotalRows, selectedRow,
               [this](int index) -> std::string { return getRowTitle(static_cast<uint8_t>(index)); },
               nullptr, nullptr,
               [this](int index) -> std::string {
                 return getRowActionName(static_cast<uint8_t>(index));
               },
               true, nullptr);

  const auto hints = mappedInput.mapLabels(tr(STR_SAVE_AND_BACK), tr(STR_CHANGE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}

const char* ReaderControlsActivity::getRowTitle(const uint8_t row) const {
  static char buf[48];
  if (row >= kTotalRows) return "";
  const char* btn;
  switch (row / 2) {
    case 0: btn = tr(STR_BACK);      break;
    case 1: btn = tr(STR_CONFIRM);   break;
    case 2: btn = tr(STR_DIR_LEFT);  break;
    case 3: btn = tr(STR_DIR_RIGHT); break;
    case 4: btn = tr(STR_DIR_UP);    break;
    case 5: btn = tr(STR_DIR_DOWN);  break;
    default: btn = "Power";          break;
  }
  const char* press = (row % 2 == 0) ? tr(STR_SHORT_PRESS) : tr(STR_LONG_PRESS);
  snprintf(buf, sizeof(buf), "%s %s", btn, press);
  return buf;
}

const char* ReaderControlsActivity::getRowActionName(const uint8_t row) const {
  if (row == kFixedRow) {
    // Power long press is always sleep; show with Fixed indicator.
    return tr(STR_READER_ACTION_SLEEP);
  }
  const auto action = getActionForRow(row);
  return actionName(action);
}

const char* ReaderControlsActivity::actionName(const CrossPointSettings::READER_ACTION action) {
  switch (action) {
    case CrossPointSettings::READER_ACTION_NONE:                  return tr(STR_READER_ACTION_NONE);
    case CrossPointSettings::READER_ACTION_PAGE_FORWARD:          return tr(STR_READER_ACTION_PAGE_FORWARD);
    case CrossPointSettings::READER_ACTION_PAGE_BACK:             return tr(STR_READER_ACTION_PAGE_BACK);
    case CrossPointSettings::READER_ACTION_SKIP_CHAPTER_FORWARD:  return tr(STR_READER_ACTION_SKIP_CHAPTER_FORWARD);
    case CrossPointSettings::READER_ACTION_SKIP_CHAPTER_BACK:     return tr(STR_READER_ACTION_SKIP_CHAPTER_BACK);
    case CrossPointSettings::READER_ACTION_OPEN_MENU:             return tr(STR_READER_ACTION_OPEN_MENU);
    case CrossPointSettings::READER_ACTION_GO_HOME:               return tr(STR_READER_ACTION_GO_HOME);
    case CrossPointSettings::READER_ACTION_FILE_BROWSER:          return tr(STR_READER_ACTION_FILE_BROWSER);
    case CrossPointSettings::READER_ACTION_SLEEP:                 return tr(STR_READER_ACTION_SLEEP);
    case CrossPointSettings::READER_ACTION_SYNC:                  return tr(STR_READER_ACTION_SYNC);
    case CrossPointSettings::READER_ACTION_BOOKMARK:              return tr(STR_READER_ACTION_BOOKMARK);
    case CrossPointSettings::READER_ACTION_FORCE_REFRESH:         return tr(STR_READER_ACTION_FORCE_REFRESH);
    case CrossPointSettings::READER_ACTION_DARK_MODE:             return tr(STR_READER_ACTION_DARK_MODE);
    case CrossPointSettings::READER_ACTION_SCREENSHOT:            return tr(STR_READER_ACTION_SCREENSHOT);
    case CrossPointSettings::READER_ACTION_MARK_FINISHED:         return tr(STR_READER_ACTION_MARK_FINISHED);
    case CrossPointSettings::READER_ACTION_FOOTNOTES:             return tr(STR_READER_ACTION_FOOTNOTES);
    case CrossPointSettings::READER_ACTION_AUTO_PAGE_TURN:        return tr(STR_READER_ACTION_AUTO_PAGE_TURN);
    case CrossPointSettings::READER_ACTION_READING_STATS:         return tr(STR_READER_ACTION_READING_STATS);
    default:                                                      return tr(STR_READER_ACTION_NONE);
  }
}

CrossPointSettings::READER_ACTION ReaderControlsActivity::getActionForRow(const uint8_t row) const {
  using A = CrossPointSettings::READER_ACTION;
  switch (row) {
    case 0:  return static_cast<A>(SETTINGS.readerShortPressBack);
    case 1:  return static_cast<A>(SETTINGS.readerLongPressBack);
    case 2:  return static_cast<A>(SETTINGS.readerShortPressConfirm);
    case 3:  return static_cast<A>(SETTINGS.readerLongPressConfirm);
    case 4:  return static_cast<A>(SETTINGS.readerShortPressLeft);
    case 5:  return static_cast<A>(SETTINGS.readerLongPressLeft);
    case 6:  return static_cast<A>(SETTINGS.readerShortPressRight);
    case 7:  return static_cast<A>(SETTINGS.readerLongPressRight);
    case 8:  return static_cast<A>(SETTINGS.readerShortPressSideUp);
    case 9:  return static_cast<A>(SETTINGS.readerLongPressSideUp);
    case 10: return static_cast<A>(SETTINGS.readerShortPressSideDown);
    case 11: return static_cast<A>(SETTINGS.readerLongPressSideDown);
    case 12: return static_cast<A>(SETTINGS.readerShortPressPower);
    case 13: return CrossPointSettings::READER_ACTION_SLEEP;
    default: return CrossPointSettings::READER_ACTION_NONE;
  }
}

void ReaderControlsActivity::cycleActionForRow(const uint8_t row) {
  if (row == kFixedRow) return;
  const auto advance = [](uint8_t& field) {
    field = (field + 1) % static_cast<uint8_t>(CrossPointSettings::READER_ACTION_COUNT);
  };
  switch (row) {
    case 0:  advance(SETTINGS.readerShortPressBack);    break;
    case 1:  advance(SETTINGS.readerLongPressBack);     break;
    case 2:  advance(SETTINGS.readerShortPressConfirm); break;
    case 3:  advance(SETTINGS.readerLongPressConfirm);  break;
    case 4:  advance(SETTINGS.readerShortPressLeft);    break;
    case 5:  advance(SETTINGS.readerLongPressLeft);     break;
    case 6:  advance(SETTINGS.readerShortPressRight);   break;
    case 7:  advance(SETTINGS.readerLongPressRight);    break;
    case 8:  advance(SETTINGS.readerShortPressSideUp);  break;
    case 9:  advance(SETTINGS.readerLongPressSideUp);   break;
    case 10: advance(SETTINGS.readerShortPressSideDown); break;
    case 11: advance(SETTINGS.readerLongPressSideDown);  break;
    case 12: advance(SETTINGS.readerShortPressPower);   break;
    default: break;
  }
}
