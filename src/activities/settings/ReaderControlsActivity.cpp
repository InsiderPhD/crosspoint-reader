#include "ReaderControlsActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

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

#if FREEINK_DEVICE_X4PRO
// X4 Pro: rows 0/2/4/6 configure the four screen swipes (left/right/up/down
// reuse the Back/Confirm/Left/Right short-press action slots). A swipe cannot
// be long-pressed, so the corresponding long-press rows are hidden. Rows 14-16
// are the screen tap zones, 17/18 the home key tap and long press; the side
// keys and Power keep their short/long pairs.
constexpr uint8_t kRowIds[] = {0, 2, 4, 6, 14, 15, 16, 17, 18, 8, 9, 10, 11, 12, 13};
#else
constexpr uint8_t kRowIds[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
#endif
constexpr uint8_t kVisibleRows = sizeof(kRowIds);
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

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), kVisibleRows, selectedRow,
                               /*hasSubtitle=*/false, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedRow = static_cast<uint8_t>(tappedIndex);
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      cycleActionForRow(kRowIds[selectedRow]);
      isDirty = true;
      requestUpdate();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    cycleActionForRow(kRowIds[selectedRow]);
    isDirty = true;
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedRow = static_cast<uint8_t>(ButtonNavigator::nextIndex(selectedRow, kVisibleRows));
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedRow = static_cast<uint8_t>(ButtonNavigator::previousIndex(selectedRow, kVisibleRows));
    requestUpdate();
  });
}

// List body between the header and the button hints. Shared by render() and
// the loop()'s tap hit-testing so the two can never disagree.
Rect ReaderControlsActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int topOffset = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - topOffset - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return Rect{0, topOffset, renderer.getScreenWidth(), contentHeight};
}

void ReaderControlsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READER_CONTROLS));

  GUI.drawList(
      renderer, listRect(), kVisibleRows, selectedRow,
      [this](int index) -> std::string { return getRowTitle(kRowIds[index]); }, nullptr, nullptr,
      [this](int index) -> std::string { return getRowActionName(kRowIds[index]); }, true, nullptr);

  const auto hints = mappedInput.mapLabels(tr(STR_SAVE_AND_BACK), tr(STR_CHANGE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}

const char* ReaderControlsActivity::getRowTitle(const uint8_t row) const {
  static char buf[48];
  if (row >= kTotalRows) return "";
#if FREEINK_DEVICE_X4PRO
  switch (row) {
    case 14:
      return tr(STR_TAP_LEFT);
    case 15:
      return tr(STR_TAP_MIDDLE);
    case 16:
      return tr(STR_TAP_RIGHT);
    case 17:
      snprintf(buf, sizeof(buf), "%s %s", tr(STR_HOME_BUTTON), tr(STR_SHORT_PRESS));
      return buf;
    case 18:
      snprintf(buf, sizeof(buf), "%s %s", tr(STR_HOME_BUTTON), tr(STR_LONG_PRESS));
      return buf;
    default:
      break;
  }
#endif
  const char* btn;
  switch (row / 2) {
#if FREEINK_DEVICE_X4PRO
    // Gestures, not buttons: swipes reuse the Back/Confirm/Left/Right action
    // slots (see MappedInputManager's X4 Pro mapping table).
    case 0:
      btn = tr(STR_SWIPE_LEFT);
      break;
    case 1:
      btn = tr(STR_SWIPE_RIGHT);
      break;
    case 2:
      btn = tr(STR_SWIPE_UP);
      break;
    case 3:
      btn = tr(STR_SWIPE_DOWN);
      break;
#else
    case 0:
      btn = tr(STR_BACK);
      break;
    case 1:
      btn = tr(STR_CONFIRM);
      break;
    case 2:
      btn = tr(STR_DIR_LEFT);
      break;
    case 3:
      btn = tr(STR_DIR_RIGHT);
      break;
#endif
    case 4:
#if FREEINK_DEVICE_X4PRO
      // X4 Pro side keys sit left/right of the screen, like the X3's.
      btn = tr(STR_DIR_SIDE_L);
#else
      // X3 side buttons sit left/right, not up/down — label them accordingly.
      btn = gpio.deviceIsX3() ? tr(STR_DIR_SIDE_L) : tr(STR_DIR_UP);
#endif
      break;
    case 5:
#if FREEINK_DEVICE_X4PRO
      btn = tr(STR_DIR_SIDE_R);
#else
      btn = gpio.deviceIsX3() ? tr(STR_DIR_SIDE_R) : tr(STR_DIR_DOWN);
#endif
      break;
    default:
      btn = "Power";
      break;
  }
#if FREEINK_DEVICE_X4PRO
  // Swipe rows are gestures — no press-type suffix.
  if (row < 8) {
    snprintf(buf, sizeof(buf), "%s", btn);
    return buf;
  }
#endif
  const char* press = (row % 2 == 0) ? tr(STR_SHORT_PRESS) : tr(STR_LONG_PRESS);
  snprintf(buf, sizeof(buf), "%s %s", btn, press);
  return buf;
}

const char* ReaderControlsActivity::getRowActionName(const uint8_t row) const {
  if (row == kFixedRow) {
    // Power long press is always sleep; show with Fixed indicator.
    return tr(STR_SLEEP);
  }
  const auto action = getActionForRow(row);
  return actionName(action);
}

const char* ReaderControlsActivity::actionName(const CrossPointSettings::READER_ACTION action) {
  switch (action) {
    case CrossPointSettings::READER_ACTION_NONE:
      return tr(STR_NONE_OPT);
    case CrossPointSettings::READER_ACTION_PAGE_FORWARD:
      return tr(STR_NEXT_PAGE);
    case CrossPointSettings::READER_ACTION_PAGE_BACK:
      return tr(STR_PREV_PAGE);
    case CrossPointSettings::READER_ACTION_SKIP_CHAPTER_FORWARD:
      return tr(STR_READER_ACTION_SKIP_CHAPTER_FORWARD);
    case CrossPointSettings::READER_ACTION_SKIP_CHAPTER_BACK:
      return tr(STR_READER_ACTION_SKIP_CHAPTER_BACK);
    case CrossPointSettings::READER_ACTION_OPEN_MENU:
      return tr(STR_READER_ACTION_OPEN_MENU);
    case CrossPointSettings::READER_ACTION_GO_HOME:
      return tr(STR_HOME);
    case CrossPointSettings::READER_ACTION_FILE_BROWSER:
      return tr(STR_READER_ACTION_FILE_BROWSER);
    case CrossPointSettings::READER_ACTION_SLEEP:
      return tr(STR_SLEEP);
    case CrossPointSettings::READER_ACTION_SYNC:
      return tr(STR_READER_ACTION_SYNC);
    case CrossPointSettings::READER_ACTION_BOOKMARK:
      return tr(STR_READER_ACTION_BOOKMARK);
    case CrossPointSettings::READER_ACTION_FORCE_REFRESH:
      return tr(STR_REFRESH);
    case CrossPointSettings::READER_ACTION_DARK_MODE:
      return tr(STR_READER_DARK_MODE);
    case CrossPointSettings::READER_ACTION_SCREENSHOT:
      return tr(STR_READER_ACTION_SCREENSHOT);
    case CrossPointSettings::READER_ACTION_MARK_FINISHED:
      return tr(STR_READER_ACTION_MARK_FINISHED);
    case CrossPointSettings::READER_ACTION_FOOTNOTES:
      return tr(STR_FOOTNOTES);
    case CrossPointSettings::READER_ACTION_AUTO_PAGE_TURN:
      return tr(STR_READER_ACTION_AUTO_PAGE_TURN);
    case CrossPointSettings::READER_ACTION_READING_STATS:
      return tr(STR_CAT_STATS);
    case CrossPointSettings::READER_ACTION_BIONIC_READING:
      return tr(STR_READER_ACTION_BIONIC_READING);
    case CrossPointSettings::READER_ACTION_BUTTON_HINTS:
      return tr(STR_READER_ACTION_BUTTON_HINTS);
    case CrossPointSettings::READER_ACTION_ROTATE_SCREEN:
      return tr(STR_READER_ACTION_ROTATE_SCREEN);
    case CrossPointSettings::READER_ACTION_CREATE_CLIPPING:
      return tr(STR_READER_ACTION_CREATE_CLIPPING);
    case CrossPointSettings::READER_ACTION_TOGGLE_BLUETOOTH:
      return tr(STR_BT_REMOTE_TOGGLE);
    default:
      return tr(STR_NONE_OPT);
  }
}

CrossPointSettings::READER_ACTION ReaderControlsActivity::getActionForRow(const uint8_t row) const {
  using A = CrossPointSettings::READER_ACTION;
  switch (row) {
    case 0:
      return static_cast<A>(SETTINGS.readerShortPressBack);
    case 1:
      return static_cast<A>(SETTINGS.readerLongPressBack);
    case 2:
      return static_cast<A>(SETTINGS.readerShortPressConfirm);
    case 3:
      return static_cast<A>(SETTINGS.readerLongPressConfirm);
    case 4:
      return static_cast<A>(SETTINGS.readerShortPressLeft);
    case 5:
      return static_cast<A>(SETTINGS.readerLongPressLeft);
    case 6:
      return static_cast<A>(SETTINGS.readerShortPressRight);
    case 7:
      return static_cast<A>(SETTINGS.readerLongPressRight);
    case 8:
      return static_cast<A>(SETTINGS.readerShortPressSideUp);
    case 9:
      return static_cast<A>(SETTINGS.readerLongPressSideUp);
    case 10:
      return static_cast<A>(SETTINGS.readerShortPressSideDown);
    case 11:
      return static_cast<A>(SETTINGS.readerLongPressSideDown);
    case 12:
      return static_cast<A>(SETTINGS.readerShortPressPower);
    case 13:
      return CrossPointSettings::READER_ACTION_SLEEP;
    case 14:
      return static_cast<A>(SETTINGS.readerTapLeft);
    case 15:
      return static_cast<A>(SETTINGS.readerTapMiddle);
    case 16:
      return static_cast<A>(SETTINGS.readerTapRight);
    case 17:
      return static_cast<A>(SETTINGS.readerShortPressHome);
    case 18:
      return static_cast<A>(SETTINGS.readerLongPressHome);
    default:
      return CrossPointSettings::READER_ACTION_NONE;
  }
}

void ReaderControlsActivity::cycleActionForRow(const uint8_t row) {
  if (row == kFixedRow) return;
  // Screenshot is a developer/testing action — skip it while cycling unless Dev Mode
  // is on (a button already set to it from a prior Dev session still works and cycles past).
  const bool dev = SETTINGS.devMode != 0;
  const auto advance = [dev](uint8_t& field) {
    do {
      field = (field + 1) % static_cast<uint8_t>(CrossPointSettings::READER_ACTION_COUNT);
    } while (!dev && field == CrossPointSettings::READER_ACTION_SCREENSHOT);
  };
  switch (row) {
    case 0:
      advance(SETTINGS.readerShortPressBack);
      break;
    case 1:
      advance(SETTINGS.readerLongPressBack);
      break;
    case 2:
      advance(SETTINGS.readerShortPressConfirm);
      break;
    case 3:
      advance(SETTINGS.readerLongPressConfirm);
      break;
    case 4:
      advance(SETTINGS.readerShortPressLeft);
      break;
    case 5:
      advance(SETTINGS.readerLongPressLeft);
      break;
    case 6:
      advance(SETTINGS.readerShortPressRight);
      break;
    case 7:
      advance(SETTINGS.readerLongPressRight);
      break;
    case 8:
      advance(SETTINGS.readerShortPressSideUp);
      break;
    case 9:
      advance(SETTINGS.readerLongPressSideUp);
      break;
    case 10:
      advance(SETTINGS.readerShortPressSideDown);
      break;
    case 11:
      advance(SETTINGS.readerLongPressSideDown);
      break;
    case 12:
      advance(SETTINGS.readerShortPressPower);
      break;
    case 14:
      advance(SETTINGS.readerTapLeft);
      break;
    case 15:
      advance(SETTINGS.readerTapMiddle);
      break;
    case 16:
      advance(SETTINGS.readerTapRight);
      break;
    case 17:
      advance(SETTINGS.readerShortPressHome);
      break;
    case 18:
      advance(SETTINGS.readerLongPressHome);
      break;
    default:
      break;
  }
}
