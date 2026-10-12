#include "StatusBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "ClockSyncActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "TimeZoneSelectActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TimeUtils.h"
#include "util/TimeZoneRegistry.h"
#include "util/TouchListNav.h"

namespace {
using CPS = CrossPointSettings;

// --- Hub rows: the three bars, then everything they share. Clock entries
// are appended only when the DS3231 RTC is present so X4 devices don't see them.
enum HubItem {
  HUB_OUTSIDE_READER = 0,  // The strip along the top of every non-reader screen
  HUB_TOP_BAR,
  HUB_BOTTOM_BAR,
  HUB_PERCENTAGE_FORMAT,
  HUB_TEXT_SIZE,
  HUB_TOP_MARGIN,
  HUB_XTC_STATUS_BAR,
  HUB_CLOCK_FORMAT,     // RTC boards only
  HUB_CLOCK_TIME_ZONE,  // RTC boards only, launches TimeZoneSelectActivity
  HUB_CLOCK_DST,        // RTC boards only
  HUB_CLOCK_SYNC,       // RTC boards only, launches ClockSyncActivity
  HUB_COUNT
};
constexpr int HUB_BASE_ITEMS = HUB_CLOCK_FORMAT;  // Rows shown on every device
const StrId hubNames[HUB_COUNT] = {
    StrId::STR_SB_OUTSIDE_READER, StrId::STR_SB_READER_TOP,        StrId::STR_SB_READER_BOTTOM,
    StrId::STR_PERCENTAGE_FORMAT, StrId::STR_STATUS_BAR_TEXT_SIZE, StrId::STR_STATUS_BAR_TOP_MARGIN,
    StrId::STR_XTC_STATUS_BAR,    StrId::STR_CLOCK_FORMAT,         StrId::STR_TIME_ZONE,
    StrId::STR_CLOCK_DST,         StrId::STR_CLOCK_SYNC_NOW,
};

// --- Bar rows: the positionable elements in display order, then the bar's own
// fill bar, then the clock (X3 only). Every element row carries a Hide / Left /
// Center / Right picker for this bar only.
constexpr uint8_t barElements[] = {
    CPS::SB_ELEM_BATTERY,           CPS::SB_ELEM_BOOK_TITLE,   CPS::SB_ELEM_CHAPTER_TITLE,
    CPS::SB_ELEM_BOOK_PERCENT,      CPS::SB_ELEM_CHAPTER_PAGE, CPS::SB_ELEM_BOOK_TIME_LEFT,
    CPS::SB_ELEM_CHAPTER_TIME_LEFT, CPS::SB_ELEM_BOOKMARK,     CPS::SB_ELEM_BLUETOOTH};
constexpr int BAR_ELEMENT_ROWS = sizeof(barElements) / sizeof(barElements[0]);
constexpr int BAR_PROGRESS_BAR = BAR_ELEMENT_ROWS;
constexpr int BAR_PROGRESS_BAR_THICKNESS = BAR_ELEMENT_ROWS + 1;
constexpr int BAR_BATTERY_STYLE = BAR_ELEMENT_ROWS + 2;  // How this bar's Battery element renders
constexpr int BAR_CLOCK = BAR_ELEMENT_ROWS + 3;          // X3 only
constexpr int BAR_BASE_ITEMS = BAR_CLOCK;
constexpr int BAR_FULL_ITEMS = BAR_CLOCK + 1;
const StrId barNames[BAR_FULL_ITEMS] = {
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
    StrId::STR_BATTERY_STYLE,
    StrId::STR_CLOCK,
};

// The STATUS_BAR_ELEMENT a bar row positions, or -1 for the fill-bar rows.
int barRowElement(const int row) {
  if (row < BAR_ELEMENT_ROWS) return barElements[row];
  if (row == BAR_CLOCK) return CPS::SB_ELEM_CLOCK;
  return -1;
}

// --- Outside Reader rows: the elements that make sense without a book, each
// with the same Hide / Left / Center / Right picker, then the battery style.
// The clock here comes from the system time, so it is offered on every board.
// No Bluetooth: the remote link is a reader-only thing.
constexpr uint8_t headerElements[] = {CPS::SB_ELEM_DATE, CPS::SB_ELEM_CLOCK, CPS::SB_ELEM_BATTERY};
constexpr int HEADER_ELEMENT_ROWS = sizeof(headerElements) / sizeof(headerElements[0]);
constexpr int HEADER_BATTERY_STYLE = HEADER_ELEMENT_ROWS;
constexpr int HEADER_ITEMS = HEADER_ELEMENT_ROWS + 1;
const StrId headerNames[HEADER_ITEMS] = {
    StrId::STR_DATE,
    StrId::STR_CLOCK,
    StrId::STR_BATTERY,
    StrId::STR_BATTERY_STYLE,
};

uint8_t& progressBarField(const bool topBar) {
  return topBar ? SETTINGS.statusBarTopProgressBar : SETTINGS.statusBarProgressBar;
}
uint8_t& progressBarThicknessField(const bool topBar) {
  return topBar ? SETTINGS.statusBarTopProgressBarThickness : SETTINGS.statusBarProgressBarThickness;
}

uint8_t& batteryStyleField(const bool topBar) {
  return SETTINGS.statusBarBatteryStyleRef(topBar ? CPS::SB_BAR_TOP : CPS::SB_BAR_BOTTOM);
}

constexpr int BATTERY_STYLE_ITEMS = CPS::STATUS_BAR_BATTERY_STYLE_COUNT;
const StrId batteryStyleNames[BATTERY_STYLE_ITEMS] = {StrId::STR_BATTERY_ICON_AND_PERCENT, StrId::STR_BATTERY_ICON_ONLY,
                                                      StrId::STR_BATTERY_PERCENT_ONLY};

constexpr int CLOCK_FORMAT_ITEMS = 2;
const StrId clockFormatNames[CLOCK_FORMAT_ITEMS] = {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H};

constexpr int CLOCK_DST_ITEMS = TimeZoneRegistry::DST_MODE_COUNT;
const StrId clockDstNames[CLOCK_DST_ITEMS] = {StrId::STR_CLOCK_DST_AUTO, StrId::STR_STATE_ON, StrId::STR_STATE_OFF};

constexpr int POSITION_ITEMS = CPS::STATUS_BAR_POS_COUNT;
const StrId positionNames[POSITION_ITEMS] = {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER,
                                             StrId::STR_ALIGN_RIGHT};

constexpr int PROGRESS_BAR_ITEMS = 3;
const StrId progressBarNames[PROGRESS_BAR_ITEMS] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};

constexpr int PROGRESS_BAR_THICKNESS_ITEMS = 3;
const StrId progressBarThicknessNames[PROGRESS_BAR_THICKNESS_ITEMS] = {
    StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK};

constexpr int PERCENTAGE_FORMAT_ITEMS = CPS::STATUS_BAR_PERCENTAGE_FORMAT_COUNT;
const StrId percentageFormatNames[PERCENTAGE_FORMAT_ITEMS] = {StrId::STR_PERCENTAGE_FORMAT_WHOLE,
                                                              StrId::STR_PERCENTAGE_FORMAT_ONE_DECIMAL,
                                                              StrId::STR_PERCENTAGE_FORMAT_TWO_DECIMALS};

constexpr int TEXT_SIZE_ITEMS = CPS::STATUS_BAR_TEXT_SIZE_COUNT;
const StrId textSizeNames[TEXT_SIZE_ITEMS] = {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE};

constexpr int XTC_STATUS_BAR_ITEMS = CPS::XTC_STATUS_BAR_MODE_COUNT;
const StrId xtcStatusBarNames[XTC_STATUS_BAR_ITEMS] = {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP,
                                                       StrId::STR_BOTH};

// Mock values so every element renders in the previews when the user places it:
// ~2h5m left for both time-left elements and a bookmarked page.
constexpr uint32_t PREVIEW_TIME_LEFT_SECONDS = 7500;
constexpr float PREVIEW_BOOK_PROGRESS = 75.25f;
constexpr int PREVIEW_PAGE = 8;
constexpr int PREVIEW_PAGE_COUNT = 32;

const int verticalPreviewPadding = 50;
const int verticalPreviewTextPadding = 40;
}  // namespace

void StatusBarSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  const bool hasClock = halClock.isAvailable();
  if (page == Page::Hub) {
    visibleItemCount = hasClock ? HUB_COUNT : HUB_BASE_ITEMS;
  } else if (page == Page::OutsideReader) {
    visibleItemCount = HEADER_ITEMS;
  } else {
    visibleItemCount = hasClock ? BAR_FULL_ITEMS : BAR_BASE_ITEMS;
  }

  // Clamp every position field on all three bars in case of corrupt/migrated data.
  for (uint8_t element = 0; element < CPS::STATUS_BAR_ELEMENT_COUNT; element++) {
    for (const CPS::STATUS_BAR_ID bar : {CPS::SB_BAR_BOTTOM, CPS::SB_BAR_TOP, CPS::SB_BAR_HEADER}) {
      uint8_t& pos = SETTINGS.statusBarPosRef(element, bar);
      if (pos >= POSITION_ITEMS) pos = CPS::SB_POS_HIDE;
    }
  }
  if (SETTINGS.statusBarHeaderBatteryStyle >= BATTERY_STYLE_ITEMS) {
    SETTINGS.statusBarHeaderBatteryStyle = CPS::SB_BATTERY_ICON_AND_PERCENT;
  }
  for (const bool top : {false, true}) {
    if (progressBarField(top) >= PROGRESS_BAR_ITEMS) {
      progressBarField(top) = CPS::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
    }
    if (progressBarThicknessField(top) >= PROGRESS_BAR_THICKNESS_ITEMS) {
      progressBarThicknessField(top) = CPS::STATUS_BAR_PROGRESS_BAR_THICKNESS::PROGRESS_BAR_NORMAL;
    }
    if (batteryStyleField(top) >= BATTERY_STYLE_ITEMS) {
      batteryStyleField(top) = CPS::SB_BATTERY_ICON_AND_PERCENT;
    }
  }
  if (SETTINGS.statusBarPercentageFormat >= PERCENTAGE_FORMAT_ITEMS) {
    SETTINGS.statusBarPercentageFormat = CPS::PERCENTAGE_WHOLE;
  }
  if (SETTINGS.statusBarTextSize >= TEXT_SIZE_ITEMS) {
    SETTINGS.statusBarTextSize = CPS::SB_TEXT_SMALL;
  }
  if (SETTINGS.statusBarTopMargin > CPS::STATUS_BAR_TOP_MARGIN_MAX) {
    SETTINGS.statusBarTopMargin = CPS::STATUS_BAR_TOP_MARGIN_MAX;
  }
  if (SETTINGS.xtcStatusBarMode >= XTC_STATUS_BAR_ITEMS) {
    SETTINGS.xtcStatusBarMode = CPS::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_HIDE;
  }
  if (SETTINGS.clockDst >= CLOCK_DST_ITEMS) {
    SETTINGS.clockDst = TimeZoneRegistry::DST_AUTO;
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
      selectedIndex = tappedIndex;
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

  // A hold turns a page, as in every other list (it used to repeat single rows).
  buttonNavigator.onNextContinuous([this, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, visibleItemCount, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, visibleItemCount, pageItems);
    requestUpdate();
  });
}

void StatusBarSettingsActivity::handleSelection() {
  if (page == Page::Hub) {
    switch (selectedIndex) {
      case HUB_OUTSIDE_READER:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput, Page::OutsideReader),
                               nullptr);
        return;
      case HUB_BOTTOM_BAR:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput, Page::BottomBar),
                               nullptr);
        return;
      case HUB_TOP_BAR:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput, Page::TopBar),
                               nullptr);
        return;
      case HUB_PERCENTAGE_FORMAT:
        SETTINGS.statusBarPercentageFormat = (SETTINGS.statusBarPercentageFormat + 1) % PERCENTAGE_FORMAT_ITEMS;
        break;
      case HUB_TEXT_SIZE:
        SETTINGS.statusBarTextSize = (SETTINGS.statusBarTextSize + 1) % TEXT_SIZE_ITEMS;
        break;
      case HUB_TOP_MARGIN:
        // Cycle 0 -> 4 -> ... -> max -> back to 0.
        if (SETTINGS.statusBarTopMargin + CPS::STATUS_BAR_TOP_MARGIN_STEP > CPS::STATUS_BAR_TOP_MARGIN_MAX) {
          SETTINGS.statusBarTopMargin = 0;
        } else {
          SETTINGS.statusBarTopMargin += CPS::STATUS_BAR_TOP_MARGIN_STEP;
        }
        break;
      case HUB_XTC_STATUS_BAR:
        SETTINGS.xtcStatusBarMode = (SETTINGS.xtcStatusBarMode + 1) % XTC_STATUS_BAR_ITEMS;
        break;
      case HUB_CLOCK_FORMAT:
        SETTINGS.clockFormat = (SETTINGS.clockFormat + 1) % CLOCK_FORMAT_ITEMS;
        break;
      case HUB_CLOCK_TIME_ZONE:
        // The picker saves and applies the zone itself, no result handler needed.
        startActivityForResult(std::make_unique<TimeZoneSelectActivity>(renderer, mappedInput), nullptr);
        return;
      case HUB_CLOCK_DST:
        // Applied lazily: every clock read goes through TimeUtils::configureTimezone().
        SETTINGS.clockDst = (SETTINGS.clockDst + 1) % CLOCK_DST_ITEMS;
        break;
      case HUB_CLOCK_SYNC:
        startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput), nullptr);
        return;
      default:
        return;
    }
    SETTINGS.saveToFile();
    return;
  }

  // Outside Reader page: the same position pickers over its own element set.
  if (page == Page::OutsideReader) {
    if (selectedIndex >= 0 && selectedIndex < HEADER_ELEMENT_ROWS) {
      uint8_t& pos = SETTINGS.statusBarPosRef(headerElements[selectedIndex], CPS::SB_BAR_HEADER);
      pos = (pos + 1) % POSITION_ITEMS;
    } else if (selectedIndex == HEADER_BATTERY_STYLE) {
      uint8_t& style = SETTINGS.statusBarBatteryStyleRef(CPS::SB_BAR_HEADER);
      style = (style + 1) % BATTERY_STYLE_ITEMS;
    } else {
      return;
    }
    SETTINGS.saveToFile();
    return;
  }

  // Bar page: position pickers cycle Hide -> Left -> Center -> Right on this bar
  // only; the fill-bar rows cycle their own enums.
  const bool top = isTopBar();
  if (const int element = barRowElement(selectedIndex); element >= 0) {
    uint8_t& pos = SETTINGS.statusBarPosRef(static_cast<uint8_t>(element), top);
    pos = (pos + 1) % POSITION_ITEMS;
  } else if (selectedIndex == BAR_PROGRESS_BAR) {
    uint8_t& mode = progressBarField(top);
    mode = (mode + 1) % PROGRESS_BAR_ITEMS;
  } else if (selectedIndex == BAR_PROGRESS_BAR_THICKNESS) {
    uint8_t& thickness = progressBarThicknessField(top);
    thickness = (thickness + 1) % PROGRESS_BAR_THICKNESS_ITEMS;
  } else if (selectedIndex == BAR_BATTERY_STYLE) {
    uint8_t& style = batteryStyleField(top);
    style = (style + 1) % BATTERY_STYLE_ITEMS;
  } else {
    return;
  }
  SETTINGS.saveToFile();
}

std::string StatusBarSettingsActivity::valueText(const int index) const {
  if (page == Page::Hub) {
    switch (index) {
      case HUB_OUTSIDE_READER:
      case HUB_BOTTOM_BAR:
      case HUB_TOP_BAR:
        return std::string();  // Opens the bar's own page.
      case HUB_PERCENTAGE_FORMAT:
        return I18N.get(percentageFormatNames[SETTINGS.statusBarPercentageFormat]);
      case HUB_TEXT_SIZE:
        return I18N.get(textSizeNames[SETTINGS.statusBarTextSize < TEXT_SIZE_ITEMS ? SETTINGS.statusBarTextSize : 0]);
      case HUB_TOP_MARGIN: {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(SETTINGS.statusBarTopMargin));
        return std::string(buf);
      }
      case HUB_XTC_STATUS_BAR:
        return I18N.get(xtcStatusBarNames[SETTINGS.xtcStatusBarMode]);
      case HUB_CLOCK_FORMAT: {
        const uint8_t fmt = SETTINGS.clockFormat < CLOCK_FORMAT_ITEMS ? SETTINGS.clockFormat : 0;
        return std::string(I18N.get(clockFormatNames[fmt]));
      }
      case HUB_CLOCK_TIME_ZONE: {
        char buf[TimeZoneRegistry::OFFSET_BUFFER_SIZE];
        return std::string(TimeUtils::getCurrentTimeZoneLabel(buf, sizeof(buf)));
      }
      case HUB_CLOCK_DST: {
        const uint8_t dst = SETTINGS.clockDst < CLOCK_DST_ITEMS ? SETTINGS.clockDst : 0;
        return std::string(I18N.get(clockDstNames[dst]));
      }
      case HUB_CLOCK_SYNC:
        return SETTINGS.clockHasBeenSynced ? tr(STR_CLOCK_SYNCED) : tr(STR_NOT_SET);
      default:
        return std::string();
    }
  }

  if (page == Page::OutsideReader) {
    if (index >= 0 && index < HEADER_ELEMENT_ROWS) {
      return I18N.get(positionNames[SETTINGS.statusBarPos(headerElements[index], CPS::SB_BAR_HEADER)]);
    }
    return I18N.get(batteryStyleNames[SETTINGS.statusBarBatteryStyleOf(CPS::SB_BAR_HEADER)]);
  }

  const bool top = isTopBar();
  if (const int element = barRowElement(index); element >= 0) {
    return I18N.get(positionNames[SETTINGS.statusBarPos(static_cast<uint8_t>(element), top)]);
  }
  if (index == BAR_PROGRESS_BAR) return I18N.get(progressBarNames[progressBarField(top)]);
  if (index == BAR_PROGRESS_BAR_THICKNESS) return I18N.get(progressBarThicknessNames[progressBarThicknessField(top)]);
  if (index == BAR_BATTERY_STYLE) return I18N.get(batteryStyleNames[batteryStyleField(top)]);
  return tr(STR_HIDE);
}

// Logical y where the top-bar preview starts: directly under the header, so it
// reads as "this is what sits above your text".
int StatusBarSettingsActivity::topPreviewY() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
}

// Logical y of the "Preview" caption above the bottom-bar preview; the list
// must end above it.
int StatusBarSettingsActivity::bottomPreviewLabelY() const {
  return renderer.getScreenHeight() - UITheme::getInstance().getStatusBarHeight() - verticalPreviewPadding -
         verticalPreviewTextPadding;
}

// List body between the header (plus the top-bar preview, when this page shows
// one and something is placed there) and the bottom preview (when this page
// shows one) or the button hints. Shared by render() and the loop()'s tap
// hit-testing so the two can never disagree.
Rect StatusBarSettingsActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int contentTop = topPreviewY();
  if (showsTopPreview()) {
    const int topBarHeight = UITheme::getTopStatusBarHeight();
    if (topBarHeight > 0) contentTop += topBarHeight + metrics.verticalSpacing;
  }
  const int contentBottom = showsBottomPreview()
                                ? bottomPreviewLabelY()
                                : renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentHeight = contentBottom - contentTop - metrics.verticalSpacing;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void StatusBarSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  StrId title = StrId::STR_CUSTOMISE_STATUS_BAR;
  const StrId* names = hubNames;
  switch (page) {
    case Page::Hub:
      break;
    case Page::BottomBar:
      title = StrId::STR_SB_READER_BOTTOM;
      names = barNames;
      break;
    case Page::TopBar:
      title = StrId::STR_SB_READER_TOP;
      names = barNames;
      break;
    case Page::OutsideReader:
      title = StrId::STR_SB_OUTSIDE_READER;
      names = headerNames;
      break;
  }
  // On the Outside Reader page this header IS the live preview: it is the very
  // strip being configured, drawn by the theme from the settings just changed.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, I18N.get(title));

  GUI.drawList(
      renderer, listRect(), visibleItemCount, static_cast<int>(selectedIndex),
      [names](int index) { return std::string(I18N.get(names[index])); }, nullptr, nullptr,
      [this](int index) { return valueText(index); }, true);

  // Draw button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Live preview of the bottom bar, lifted clear of the button hints.
  if (showsBottomPreview()) {
    GUI.drawStatusBar(renderer, PREVIEW_BOOK_PROGRESS, PREVIEW_PAGE, PREVIEW_PAGE_COUNT, tr(STR_EXAMPLE_BOOK),
                      tr(STR_EXAMPLE_CHAPTER), PREVIEW_TIME_LEFT_SECONDS, PREVIEW_TIME_LEFT_SECONDS,
                      /*isPageBookmarked=*/true, verticalPreviewPadding);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, bottomPreviewLabelY(), tr(STR_PREVIEW));
  }

  // Live preview of the top bar, just under the header, where it would sit
  // above the page text. edgePadding pushes it down from the bezel margin the
  // bar normally hangs off to the header's bottom edge.
  if (showsTopPreview() && UITheme::getTopStatusBarHeight() > 0) {
    int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
    renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                     &orientedMarginLeft);
    const int edgePadding = std::max(0, topPreviewY() - orientedMarginTop);
    GUI.drawStatusBarSide(renderer, StatusBarSide::Top, StatusBarSide::Top, PREVIEW_BOOK_PROGRESS, PREVIEW_PAGE,
                          PREVIEW_PAGE_COUNT, tr(STR_EXAMPLE_BOOK), tr(STR_EXAMPLE_CHAPTER), PREVIEW_TIME_LEFT_SECONDS,
                          PREVIEW_TIME_LEFT_SECONDS, /*isPageBookmarked=*/true, edgePadding);
  }

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
