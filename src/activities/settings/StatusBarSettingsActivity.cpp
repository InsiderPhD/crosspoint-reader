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

// --- Hub rows: the two bars, then everything both bars share. Clock entries
// are appended only when the DS3231 RTC is present so X4 devices don't see them.
enum HubItem {
  HUB_BOTTOM_BAR = 0,
  HUB_TOP_BAR,
  HUB_PERCENTAGE_FORMAT,
  HUB_TOP_MARGIN,
  HUB_XTC_STATUS_BAR,
  HUB_CLOCK_OUTSIDE_READER,  // Time after the date on the non-reader headers
  HUB_CLOCK_FORMAT,          // RTC boards only
  HUB_CLOCK_TIME_ZONE,       // RTC boards only, launches TimeZoneSelectActivity
  HUB_CLOCK_DST,             // RTC boards only
  HUB_CLOCK_SYNC,            // RTC boards only, launches ClockSyncActivity
  HUB_COUNT
};
constexpr int HUB_BASE_ITEMS = HUB_CLOCK_FORMAT;  // Rows shown on every device
const StrId hubNames[HUB_COUNT] = {
    StrId::STR_BOTTOM_STATUS_BAR,     StrId::STR_TOP_STATUS_BAR, StrId::STR_PERCENTAGE_FORMAT,
    StrId::STR_STATUS_BAR_TOP_MARGIN, StrId::STR_XTC_STATUS_BAR, StrId::STR_CLOCK_OUTSIDE_READER,
    StrId::STR_CLOCK_FORMAT,          StrId::STR_TIME_ZONE,      StrId::STR_CLOCK_DST,
    StrId::STR_CLOCK_SYNC_NOW,
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

uint8_t& progressBarField(const bool topBar) {
  return topBar ? SETTINGS.statusBarTopProgressBar : SETTINGS.statusBarProgressBar;
}
uint8_t& progressBarThicknessField(const bool topBar) {
  return topBar ? SETTINGS.statusBarTopProgressBarThickness : SETTINGS.statusBarProgressBarThickness;
}

uint8_t& batteryStyleField(const bool topBar) {
  return topBar ? SETTINGS.statusBarTopBatteryStyle : SETTINGS.statusBarBatteryStyle;
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
  } else {
    visibleItemCount = hasClock ? BAR_FULL_ITEMS : BAR_BASE_ITEMS;
  }

  // Clamp every position field on both bars in case of corrupt/migrated data.
  for (uint8_t element = 0; element < CPS::STATUS_BAR_ELEMENT_COUNT; element++) {
    for (const bool top : {false, true}) {
      uint8_t& pos = SETTINGS.statusBarPosRef(element, top);
      if (pos >= POSITION_ITEMS) pos = CPS::SB_POS_HIDE;
    }
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
  if (page == Page::Hub) {
    switch (selectedIndex) {
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
      case HUB_CLOCK_OUTSIDE_READER:
        SETTINGS.showClockOutsideReader = SETTINGS.showClockOutsideReader ? 0 : 1;
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
      case HUB_BOTTOM_BAR:
      case HUB_TOP_BAR:
        return std::string();  // Opens the bar's own page.
      case HUB_PERCENTAGE_FORMAT:
        return I18N.get(percentageFormatNames[SETTINGS.statusBarPercentageFormat]);
      case HUB_TOP_MARGIN: {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(SETTINGS.statusBarTopMargin));
        return std::string(buf);
      }
      case HUB_XTC_STATUS_BAR:
        return I18N.get(xtcStatusBarNames[SETTINGS.xtcStatusBarMode]);
      case HUB_CLOCK_OUTSIDE_READER:
        return SETTINGS.showClockOutsideReader ? tr(STR_SHOW) : tr(STR_HIDE);
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
  if (page != Page::BottomBar) {
    const int topBarHeight = UITheme::getTopStatusBarHeight();
    if (topBarHeight > 0) contentTop += topBarHeight + metrics.verticalSpacing;
  }
  const int contentBottom = page == Page::TopBar
                                ? renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing
                                : bottomPreviewLabelY();
  const int contentHeight = contentBottom - contentTop - metrics.verticalSpacing;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void StatusBarSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  const StrId title = page == Page::Hub         ? StrId::STR_CUSTOMISE_STATUS_BAR
                      : page == Page::BottomBar ? StrId::STR_BOTTOM_STATUS_BAR
                                                : StrId::STR_TOP_STATUS_BAR;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, I18N.get(title));

  const StrId* names = page == Page::Hub ? hubNames : barNames;
  GUI.drawList(
      renderer, listRect(), visibleItemCount, static_cast<int>(selectedIndex),
      [names](int index) { return std::string(I18N.get(names[index])); }, nullptr, nullptr,
      [this](int index) { return valueText(index); }, true);

  // Draw button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Live preview of the bottom bar, lifted clear of the button hints.
  if (page != Page::TopBar) {
    GUI.drawStatusBar(renderer, PREVIEW_BOOK_PROGRESS, PREVIEW_PAGE, PREVIEW_PAGE_COUNT, tr(STR_EXAMPLE_BOOK),
                      tr(STR_EXAMPLE_CHAPTER), PREVIEW_TIME_LEFT_SECONDS, PREVIEW_TIME_LEFT_SECONDS,
                      /*isPageBookmarked=*/true, verticalPreviewPadding);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, bottomPreviewLabelY(), tr(STR_PREVIEW));
  }

  // Live preview of the top bar, just under the header, where it would sit
  // above the page text. edgePadding pushes it down from the bezel margin the
  // bar normally hangs off to the header's bottom edge.
  if (page != Page::BottomBar && UITheme::getTopStatusBarHeight() > 0) {
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
