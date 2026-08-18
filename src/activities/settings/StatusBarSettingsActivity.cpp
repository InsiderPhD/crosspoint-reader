#include "StatusBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <cstring>
#include <memory>

#include "ClockOffsetActivity.h"
#include "ClockSyncActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

namespace {
// Menu items in their natural order. Clock entries are appended only when the
// DS3231 RTC is present so X4 devices don't see them at all.
//
// Every content element carries a Hide/Left/Middle/Right position picker; the
// progress fill bar keeps its own Book/Chapter/Hide + thickness pair.
enum MenuItem {
  ITEM_BATTERY_POS = 0,
  ITEM_BOOK_TITLE_POS,
  ITEM_CHAPTER_TITLE_POS,
  ITEM_BOOK_PERCENT_POS,
  ITEM_CHAPTER_PAGE_POS,
  ITEM_BOOK_TIME_LEFT_POS,
  ITEM_CHAPTER_TIME_LEFT_POS,
  ITEM_BOOKMARK_POS,
  ITEM_BLUETOOTH_POS,
  ITEM_PROGRESS_BAR,
  ITEM_PROGRESS_BAR_THICKNESS,
  ITEM_TOP_MARGIN,
  ITEM_XTC_STATUS_BAR,
  ITEM_CLOCK_POS,         // X3 only
  ITEM_CLOCK_FORMAT,      // X3 only
  ITEM_CLOCK_UTC_OFFSET,  // X3 only, launches ClockOffsetActivity
  ITEM_CLOCK_SYNC,        // X3 only, launches ClockSyncActivity
  ITEM_COUNT
};

constexpr int BASE_MENU_ITEMS = ITEM_CLOCK_POS;  // Items shown on every device
constexpr int FULL_MENU_ITEMS = ITEM_COUNT;      // Items shown when RTC is available

const StrId menuNames[FULL_MENU_ITEMS] = {
    StrId::STR_BATTERY,
    StrId::STR_BOOK_TITLE,
    StrId::STR_CHAPTER_TITLE,
    StrId::STR_BOOK_PROGRESS_PERCENTAGE,
    StrId::STR_CHAPTER_PAGE_COUNT,
    StrId::STR_BOOK_TIME_LEFT,
    StrId::STR_CHAPTER_TIME_LEFT,
    StrId::STR_BOOKMARK,
    StrId::STR_BLUETOOTH,
    StrId::STR_PROGRESS_BAR,
    StrId::STR_PROGRESS_BAR_THICKNESS,
    StrId::STR_STATUS_BAR_TOP_MARGIN,
    StrId::STR_XTC_STATUS_BAR,
    StrId::STR_CLOCK,
    StrId::STR_CLOCK_FORMAT,
    StrId::STR_CLOCK_UTC_OFFSET,
    StrId::STR_CLOCK_SYNC_NOW,
};

constexpr int CLOCK_FORMAT_ITEMS = 2;
const StrId clockFormatNames[CLOCK_FORMAT_ITEMS] = {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H};

std::string formatUtcOffset(uint8_t biasedQ) {
  // biasedQ is in quarter-hour steps, biased by 48 (so 48 = UTC+0).
  if (biasedQ > 104) biasedQ = 48;
  int totalMinutes = (static_cast<int>(biasedQ) - 48) * 15;
  bool neg = totalMinutes < 0;
  int absMinutes = neg ? -totalMinutes : totalMinutes;
  int hours = absMinutes / 60;
  int mins = absMinutes % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "UTC%c%d:%02d", neg ? '-' : '+', hours, mins);
  return buf;
}

// Position picker labels: Hide / Left / Center / Right (indexed by STATUS_BAR_POS).
constexpr int POSITION_ITEMS = CrossPointSettings::STATUS_BAR_POS_COUNT;
const StrId positionNames[POSITION_ITEMS] = {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER,
                                             StrId::STR_ALIGN_RIGHT};

constexpr int PROGRESS_BAR_ITEMS = 3;
const StrId progressBarNames[PROGRESS_BAR_ITEMS] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};

constexpr int PROGRESS_BAR_THICKNESS_ITEMS = 3;
const StrId progressBarThicknessNames[PROGRESS_BAR_THICKNESS_ITEMS] = {
    StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK};

constexpr int XTC_STATUS_BAR_ITEMS = 3;
const StrId xtcStatusBarNames[XTC_STATUS_BAR_ITEMS] = {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP};

const int verticalPreviewPadding = 50;
const int verticalPreviewTextPadding = 40;

// Maps a menu item to the settings position field it controls, or nullptr if the
// item is not a position picker (progress bar, clock format, launchers, ...).
uint8_t* positionFieldFor(int item) {
  switch (item) {
    case ITEM_BATTERY_POS:
      return &SETTINGS.statusBarBatteryPos;
    case ITEM_BOOK_TITLE_POS:
      return &SETTINGS.statusBarBookTitlePos;
    case ITEM_CHAPTER_TITLE_POS:
      return &SETTINGS.statusBarChapterTitlePos;
    case ITEM_BOOK_PERCENT_POS:
      return &SETTINGS.statusBarBookPercentPos;
    case ITEM_CHAPTER_PAGE_POS:
      return &SETTINGS.statusBarChapterPagePos;
    case ITEM_BOOK_TIME_LEFT_POS:
      return &SETTINGS.statusBarBookTimeLeftPos;
    case ITEM_CHAPTER_TIME_LEFT_POS:
      return &SETTINGS.statusBarChapterTimeLeftPos;
    case ITEM_BOOKMARK_POS:
      return &SETTINGS.statusBarBookmarkPos;
    case ITEM_BLUETOOTH_POS:
      return &SETTINGS.statusBarBluetoothPos;
    case ITEM_CLOCK_POS:
      return &SETTINGS.statusBarClockPos;
    default:
      return nullptr;
  }
}
}  // namespace

void StatusBarSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  visibleItemCount = halClock.isAvailable() ? FULL_MENU_ITEMS : BASE_MENU_ITEMS;

  // Clamp every position field in case of corrupt/migrated data.
  for (int item = 0; item < FULL_MENU_ITEMS; item++) {
    if (uint8_t* pos = positionFieldFor(item)) {
      if (*pos >= POSITION_ITEMS) *pos = CrossPointSettings::SB_POS_HIDE;
    }
  }

  if (SETTINGS.statusBarProgressBar >= PROGRESS_BAR_ITEMS) {
    SETTINGS.statusBarProgressBar = CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  }
  if (SETTINGS.statusBarProgressBarThickness >= PROGRESS_BAR_THICKNESS_ITEMS) {
    SETTINGS.statusBarProgressBarThickness = CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS::PROGRESS_BAR_NORMAL;
  }
  if (SETTINGS.statusBarTopMargin > CrossPointSettings::STATUS_BAR_TOP_MARGIN_MAX) {
    SETTINGS.statusBarTopMargin = CrossPointSettings::STATUS_BAR_TOP_MARGIN_MAX;
  }
  if (SETTINGS.xtcStatusBarMode >= XTC_STATUS_BAR_ITEMS) {
    SETTINGS.xtcStatusBarMode = CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_HIDE;
  }
  if (SETTINGS.clockUtcOffsetQ > 104) {
    SETTINGS.clockUtcOffsetQ = 48;  // Default to UTC+0
  }
  if (SETTINGS.clockFormat >= CLOCK_FORMAT_ITEMS) {
    SETTINGS.clockFormat = 0;
  }

  requestUpdate();
}

void StatusBarSettingsActivity::onExit() { Activity::onExit(); }

void StatusBarSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), visibleItemCount, selectedIndex,
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

  // Full Touch: a vertical swipe turns a page, matching the held side key.
  const int pageItems = GUI.listGeometry(listRect(), selectedIndex, /*hasSubtitle=*/false).pageItems;
  if (TouchListNav::pageSwipe(mappedInput, visibleItemCount, pageItems, selectedIndex)) {
    requestUpdate();
    return;
  }

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, visibleItemCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, visibleItemCount);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, visibleItemCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, visibleItemCount);
    requestUpdate();
  });
}

void StatusBarSettingsActivity::handleSelection() {
  // Position pickers all cycle Hide -> Left -> Middle -> Right.
  if (uint8_t* pos = positionFieldFor(selectedIndex)) {
    *pos = (*pos + 1) % POSITION_ITEMS;
    SETTINGS.saveToFile();
    return;
  }

  switch (selectedIndex) {
    case ITEM_PROGRESS_BAR:
      SETTINGS.statusBarProgressBar = (SETTINGS.statusBarProgressBar + 1) % PROGRESS_BAR_ITEMS;
      break;
    case ITEM_PROGRESS_BAR_THICKNESS:
      SETTINGS.statusBarProgressBarThickness =
          (SETTINGS.statusBarProgressBarThickness + 1) % PROGRESS_BAR_THICKNESS_ITEMS;
      break;
    case ITEM_TOP_MARGIN:
      // Cycle 0 -> 4 -> ... -> max -> back to 0.
      if (SETTINGS.statusBarTopMargin + CrossPointSettings::STATUS_BAR_TOP_MARGIN_STEP >
          CrossPointSettings::STATUS_BAR_TOP_MARGIN_MAX) {
        SETTINGS.statusBarTopMargin = 0;
      } else {
        SETTINGS.statusBarTopMargin += CrossPointSettings::STATUS_BAR_TOP_MARGIN_STEP;
      }
      break;
    case ITEM_XTC_STATUS_BAR:
      SETTINGS.xtcStatusBarMode = (SETTINGS.xtcStatusBarMode + 1) % XTC_STATUS_BAR_ITEMS;
      break;
    case ITEM_CLOCK_FORMAT:
      SETTINGS.clockFormat = (SETTINGS.clockFormat + 1) % CLOCK_FORMAT_ITEMS;
      break;
    case ITEM_CLOCK_UTC_OFFSET:
      // Launch the dedicated offset picker. It saves on exit, no result handler needed.
      startActivityForResult(std::make_unique<ClockOffsetActivity>(renderer, mappedInput), nullptr);
      return;
    case ITEM_CLOCK_SYNC:
      startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput), nullptr);
      return;
    default:
      return;
  }
  SETTINGS.saveToFile();
}

// List body between the header and the button hints. Shared by render() and
// the loop()'s tap hit-testing so the two can never disagree.
Rect StatusBarSettingsActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void StatusBarSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CUSTOMISE_STATUS_BAR));

  GUI.drawList(
      renderer, listRect(), visibleItemCount, static_cast<int>(selectedIndex),
      [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr, nullptr,
      [](int index) -> std::string {
        if (const uint8_t* pos = positionFieldFor(index)) {
          const uint8_t v = *pos < POSITION_ITEMS ? *pos : CrossPointSettings::SB_POS_HIDE;
          return I18N.get(positionNames[v]);
        }
        switch (index) {
          case ITEM_PROGRESS_BAR:
            return I18N.get(progressBarNames[SETTINGS.statusBarProgressBar]);
          case ITEM_PROGRESS_BAR_THICKNESS:
            return I18N.get(progressBarThicknessNames[SETTINGS.statusBarProgressBarThickness]);
          case ITEM_TOP_MARGIN: {
            char buf[8];
            snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(SETTINGS.statusBarTopMargin));
            return std::string(buf);
          }
          case ITEM_XTC_STATUS_BAR:
            return I18N.get(xtcStatusBarNames[SETTINGS.xtcStatusBarMode]);
          case ITEM_CLOCK_FORMAT: {
            const uint8_t fmt = SETTINGS.clockFormat < CLOCK_FORMAT_ITEMS ? SETTINGS.clockFormat : 0;
            return std::string(I18N.get(clockFormatNames[fmt]));
          }
          case ITEM_CLOCK_UTC_OFFSET:
            return formatUtcOffset(SETTINGS.clockUtcOffsetQ);
          case ITEM_CLOCK_SYNC:
            return SETTINGS.clockHasBeenSynced ? tr(STR_CLOCK_SYNCED) : tr(STR_NOT_SET);
          default:
            return tr(STR_HIDE);
        }
      },
      true);

  // Draw button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Live preview: mock time-left for both elements (~2h5m) and a bookmarked page
  // so every element renders when the user places it.
  constexpr uint32_t PREVIEW_TIME_LEFT_SECONDS = 7500;
  GUI.drawStatusBar(renderer, /*bookProgress=*/75, /*currentPage=*/8, /*pageCount=*/32, tr(STR_EXAMPLE_BOOK),
                    tr(STR_EXAMPLE_CHAPTER), /*chapterTimeLeftSeconds=*/PREVIEW_TIME_LEFT_SECONDS,
                    /*bookTimeLeftSeconds=*/PREVIEW_TIME_LEFT_SECONDS, /*isPageBookmarked=*/true,
                    verticalPreviewPadding);

  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding,
                    renderer.getScreenHeight() - UITheme::getInstance().getStatusBarHeight() - verticalPreviewPadding -
                        verticalPreviewTextPadding,
                    tr(STR_PREVIEW));

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
