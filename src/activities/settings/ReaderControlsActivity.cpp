#include "ReaderControlsActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#if FREEINK_DEVICE_X4PRO
#include "ReaderActionSelectActivity.h"
#endif
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
// are the screen tap zones with 19-21 their hold (long-press) variants shown
// as tap/hold pairs, 17/18 the home key tap and long press; the side keys and
// Power keep their short/long pairs.
constexpr uint8_t kRowIds[] = {0, 2, 4, 6, 14, 19, 15, 20, 16, 21, 17, 18, 8, 9, 10, 11, 12, 13};
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
      activateRow(kRowIds[selectedRow]);
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateRow(kRowIds[selectedRow]);
    return;
  }

  // Full Touch: a vertical swipe turns a page, matching the held side key.
  const int pageItems = GUI.listGeometry(listRect(), selectedRow, /*hasSubtitle=*/false).pageItems;
  int swipeIndex = selectedRow;
  if (TouchListNav::pageSwipe(mappedInput, kVisibleRows, pageItems, swipeIndex)) {
    selectedRow = static_cast<uint8_t>(swipeIndex);
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
    case 19:
      return tr(STR_HOLD_LEFT);
    case 20:
      return tr(STR_HOLD_MIDDLE);
    case 21:
      return tr(STR_HOLD_RIGHT);
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
    // The front four. Back and Confirm are reached by the horizontal swipes,
    // which read backwards from the slot names -- on hardware a RIGHTWARD flick
    // lands on the Back slot and a leftward one on Confirm -- so those rows are
    // labelled by the gesture the user actually makes, not by the slot behind
    // it. Left/Right are the action bar's outer slots. The VERTICAL swipes are
    // not here: they are the side keys' gesture twin and follow the side rows
    // below. See MappedInputManager's X4 Pro mapping table.
    case 0:
      btn = tr(STR_SWIPE_RIGHT);
      break;
    case 1:
      btn = tr(STR_SWIPE_LEFT);
      break;
    case 2:
      btn = tr(STR_DIR_LEFT);
      break;
    case 3:
      btn = tr(STR_DIR_RIGHT);
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
      // X4 Pro side keys sit left/right of the screen, like the X3's, and a
      // vertical swipe is their gesture twin -- both reach this row.
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
  if (row == 18) {
    // Home hold is hard-wired to the reader menu (see
    // CrossPointSettings::effectiveReaderLongPressHome).
    return tr(STR_READER_ACTION_OPEN_MENU);
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
    case CrossPointSettings::READER_ACTION_SYNC:
      return tr(STR_READER_ACTION_SYNC);
    case CrossPointSettings::READER_ACTION_BOOKMARK:
      return tr(STR_READER_ACTION_BOOKMARK);
    case CrossPointSettings::READER_ACTION_FORCE_REFRESH:
      return tr(STR_REFRESH);
    case CrossPointSettings::READER_ACTION_DARK_MODE:
      // Short variant; settings/menu keep the full STR_READER_DARK_MODE wording.
      return tr(STR_READER_ACTION_DARK_MODE);
    case CrossPointSettings::READER_ACTION_SCREENSHOT:
      return tr(STR_READER_ACTION_SCREENSHOT);
    case CrossPointSettings::READER_ACTION_FOOTNOTES:
      // Short variant; the reader menu keeps the full STR_FOOTNOTES wording.
      return tr(STR_READER_ACTION_FOOTNOTES);
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
      // Deliberately short (like the other action labels); the reader menu row
      // keeps the full STR_BT_REMOTE_TOGGLE wording.
      return tr(STR_READER_ACTION_BLUETOOTH);
    case CrossPointSettings::READER_ACTION_HIDE_STATUS_BAR:
      return tr(STR_READER_ACTION_STATUS_BAR);
    case CrossPointSettings::READER_ACTION_DICTIONARY:
      return tr(STR_LOOKUP);
    default:
      return tr(STR_NONE_OPT);
  }
}

uint8_t* ReaderControlsActivity::fieldForRow(const uint8_t row) const {
  switch (row) {
    case 0:
      return &SETTINGS.readerShortPressBack;
    case 1:
      return &SETTINGS.readerLongPressBack;
    case 2:
      return &SETTINGS.readerShortPressConfirm;
    case 3:
      return &SETTINGS.readerLongPressConfirm;
    case 4:
      return &SETTINGS.readerShortPressLeft;
    case 5:
      return &SETTINGS.readerLongPressLeft;
    case 6:
      return &SETTINGS.readerShortPressRight;
    case 7:
      return &SETTINGS.readerLongPressRight;
    case 8:
      return &SETTINGS.readerShortPressSideUp;
    case 9:
      return &SETTINGS.readerLongPressSideUp;
    case 10:
      return &SETTINGS.readerShortPressSideDown;
    case 11:
      return &SETTINGS.readerLongPressSideDown;
    case 12:
      return &SETTINGS.readerShortPressPower;
    case 13:
      // Power long press is fixed sleep and isn't a bindable action; the row's
      // label is special-cased in getRowActionName().
      return nullptr;
    case 14:
      return &SETTINGS.readerTapLeft;
    case 15:
      return &SETTINGS.readerTapMiddle;
    case 16:
      return &SETTINGS.readerTapRight;
    case 17:
      return &SETTINGS.readerShortPressHome;
    case 18:
      // Hard-wired to Open Menu: the menu is the only guaranteed route to Go
      // Home once every other slot is remappable. (Dev-mode override parked --
      // see CrossPointSettings::effectiveReaderLongPressHome.)
      return nullptr;
    case 19:
      return &SETTINGS.readerHoldLeft;
    case 20:
      return &SETTINGS.readerHoldMiddle;
    case 21:
      return &SETTINGS.readerHoldRight;
    default:
      return nullptr;
  }
}

CrossPointSettings::READER_ACTION ReaderControlsActivity::getActionForRow(const uint8_t row) const {
  const uint8_t* field = fieldForRow(row);
  return field ? static_cast<CrossPointSettings::READER_ACTION>(*field) : CrossPointSettings::READER_ACTION_NONE;
}

void ReaderControlsActivity::activateRow(const uint8_t row) {
#if FREEINK_DEVICE_X4PRO
  openActionPicker(row);
#else
  cycleActionForRow(row);
  isDirty = true;
  requestUpdate();
#endif
}

#if FREEINK_DEVICE_X4PRO

// X4 Pro only. Cycling costs one Confirm per step, which is the cheaper
// interaction on a device with real front buttons -- a picker would turn every
// change into open/scroll/select and wear those buttons three times as fast.
// The X4 Pro has no front buttons: the same cycle is a blind tap-and-look, so
// there it becomes a list.
void ReaderControlsActivity::openActionPicker(const uint8_t row) {
  uint8_t* field = fieldForRow(row);
  if (!field) return;

  startActivityForResult(std::make_unique<ReaderActionSelectActivity>(renderer, mappedInput, getRowTitle(row), *field),
                         [this, field](const ActivityResult& result) {
                           const auto* picked = std::get_if<ReaderActionResult>(&result.data);
                           if (!picked || picked->action == *field) return;
                           *field = picked->action;
                           // Batched: the single save happens when this screen exits.
                           isDirty = true;
                         });
}

#else

void ReaderControlsActivity::cycleActionForRow(const uint8_t row) {
  uint8_t* field = fieldForRow(row);
  if (!field) return;
  // Screenshot is a developer/testing action -- skip it while cycling unless Dev Mode
  // is on (a button already set to it from a prior Dev session still works and cycles past).
  // Retired values (Sleep, Mark Finished) are never offered.
  const bool dev = SETTINGS.devMode != 0;
  do {
    *field = (*field + 1) % static_cast<uint8_t>(CrossPointSettings::READER_ACTION_COUNT);
  } while (CrossPointSettings::isRetiredReaderAction(*field) ||
           (!dev && *field == CrossPointSettings::READER_ACTION_SCREENSHOT));
}

#endif  // FREEINK_DEVICE_X4PRO
