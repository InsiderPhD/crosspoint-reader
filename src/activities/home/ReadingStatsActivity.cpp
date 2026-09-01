#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <string>
#include <vector>

#include "AppMetricCard.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "ReadingDayDetailActivity.h"
#include "ReadingStatsDetailActivity.h"
#include "ReadingStatsStore.h"
#include "SessionDateEditActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/ActionBar.h"
#include "components/UITheme.h"
#include "components/icons/award24.h"
#include "components/icons/book24.h"
#include "components/icons/check24.h"
#include "components/icons/checkbox24.h"
#include "components/icons/confetti24.h"
#include "components/icons/files24.h"
#include "components/icons/last30days24.h"
#include "components/icons/last7days24.h"
#include "components/icons/readingtime24.h"
#include "components/icons/receipttotal24.h"
#include "components/icons/recent.h"
#include "components/icons/streak24.h"
#include "fontIds.h"
#include "util/ReadingStatsAnalytics.h"
#include "util/TimeUtils.h"
#include "util/TouchListNav.h"

namespace {
constexpr unsigned long BOOK_LONG_PRESS_MS = 1000;
constexpr int TOTAL_STATS_PAGES = 6;
constexpr int PAGE_OVERVIEW = 0;
constexpr int PAGE_STARTED_BOOKS = 1;
constexpr int PAGE_WEEKLY = 2;
constexpr int PAGE_MONTHLY = 3;
constexpr int PAGE_SESSIONS = 4;
constexpr int PAGE_YEAR = 5;

// Tab labels in display order — index matches the PAGE_* enum values above.
// The Reading Profile is shown on the Overview tab (not a dedicated tab). The
// tab bar auto-scrolls to keep the selected tab's full label readable.
constexpr StrId TAB_NAMES[TOTAL_STATS_PAGES] = {
    StrId::STR_STATS_TAB_OVERVIEW, StrId::STR_STATS_TAB_BOOKS, StrId::STR_STATS_TAB_WEEKLY, StrId::STR_MONTH,
    StrId::STR_STATS_TAB_SESSIONS, StrId::STR_STATS_TAB_YEAR,
};

// Sessions tab is capped at one screenful like the Books tab.
constexpr int SESSIONS_PER_PAGE = 4;

// Pixels a scrollable tab moves per Up/Down/Left/Right press (~2 stat rows).
constexpr int STATS_SCROLL_STEP = 68;
// A swipe moves a screenful rather than the single step a key press gives —
// that is the gesture's whole point on a long stats page. Deliberately several
// key-steps rather than the exact viewport height: the viewport is computed
// inside render() and is not worth threading out for this.
constexpr int STATS_SCROLL_PAGE = STATS_SCROLL_STEP * 4;

// Collects sessionLog indices for entries that don't yet have a date
// assigned. The Sessions tab is intentionally a "needs your input" inbox —
// once the user picks a date for a session via the editor, editSessionDate
// sets dayOrdinal != 0 and the entry drops out of this list. Returned in
// reverse order so the most recently recorded ones surface at the top.
std::vector<size_t> collectUndatedSessionIndices() {
  const auto& log = READING_STATS.getSessionLog();
  std::vector<size_t> indices;
  indices.reserve(log.size());
  for (size_t i = log.size(); i-- > 0;) {
    if (log[i].dayOrdinal == 0) {
      indices.push_back(i);
    }
  }
  return indices;
}

constexpr int SUMMARY_ROW_HEIGHT = 34;
constexpr int SUMMARY_GAP = 8;
constexpr int LIST_HEADER_HEIGHT = 34;
constexpr int LIST_HEADER_BOTTOM_GAP = 12;
constexpr int BOOK_ROW_HEIGHT = 82;
constexpr int BOOK_ROW_GAP = 10;
constexpr int BOOKS_PER_PAGE = 4;
constexpr int CHART_HEIGHT = 170;
constexpr int CHART_HEADER_HEIGHT = 34;
constexpr int CHART_TOP_GAP = 8;
constexpr int SECTION_GAP = 10;
// Sessions tab: shared between render() and listRect() (the inbox list sits
// below the three bucket cards).
constexpr int BUCKET_CARD_HEIGHT = 60;
constexpr int MONTH_HEADER_HEIGHT = 34;
constexpr int HEATMAP_GRID_GAP = 6;
constexpr int LEGEND_HEIGHT = 30;
constexpr int LEGEND_SWATCH_SIZE = 16;

constexpr int SELECTION_SIDE_WIDTH = 8;
constexpr int SELECTION_CAP_HEIGHT = 8;
constexpr int SELECTION_RADIUS = 6;

struct ChartBar {
  std::string bottomLabel;
  std::string topLabel;
  uint64_t readingMs = 0;
};

struct HeatmapCell {
  uint32_t dayOrdinal = 0;
  uint64_t readingMs = 0;
  unsigned day = 0;
  bool inViewedMonth = false;
  bool isReferenceDay = false;
};

struct MonthSummary {
  uint64_t monthTotalReadingMs = 0;
  uint64_t yearTotalReadingMs = 0;
  uint64_t bestDayReadingMs = 0;
  uint32_t monthDaysRead = 0;
  unsigned bestDayOfMonth = 0;
};

void captureFirstStatsAccessDate() {
  // Keep this path network-free. Calling NTP before networking is initialized
  // can trigger lwIP asserts on some boots.
  static bool attemptedThisBoot = false;
  if (attemptedThisBoot || TimeUtils::isClockValid(APP_STATE.lastKnownValidTimestamp)) {
    return;
  }
  attemptedThisBoot = true;

  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  if (!TimeUtils::isClockValid(now)) {
    return;
  }

  APP_STATE.lastKnownValidTimestamp = now;
  APP_STATE.saveToFile();
}

std::vector<const ReadingBookStats*> getUnfinishedBooks() {
  std::vector<const ReadingBookStats*> unfinished;
  const auto& books = READING_STATS.getBooks();
  unfinished.reserve(books.size());
  for (const auto& book : books) {
    if (book.lastProgressPercent < 95) {
      unfinished.push_back(&book);
    }
  }
  return unfinished;
}

std::string getBookTitle(const ReadingBookStats& book) { return book.title.empty() ? book.path : book.title; }

std::string getBookSubtitle(const ReadingBookStats& book) {
  if (!book.author.empty()) {
    return book.author;
  }
  return book.completed ? std::string(tr(STR_DONE)) : std::string(tr(STR_IN_PROGRESS));
}

void drawMetricRow(GfxRenderer& renderer, const Rect& rect, const uint8_t* icon, const char* label,
                   const std::string& value) {
  constexpr int iconSize = 24;
  constexpr int iconPad = 5;
  constexpr int textY = 6;
  renderer.drawIcon(icon, rect.x, rect.y + iconPad, iconSize, iconSize);
  renderer.drawText(UI_10_FONT_ID, rect.x + iconSize + 10, rect.y + textY, label);
  const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, rect.x + rect.width - valueWidth, rect.y + textY, value.c_str(), true,
                    EpdFontFamily::BOLD);
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1);
}

// Sakamoto's algorithm: 0 = Sunday .. 6 = Saturday.
int statsDayOfWeek(int year, unsigned month, unsigned day) {
  static constexpr int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 3) year -= 1;
  return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + static_cast<int>(day)) % 7;
}

// Defined further down in the heatmap section; used by the week cells so they
// share the Monthly view's goal-scaled heat levels.
int getHeatLevel(uint64_t readingMs);

// One large weekday cell shaded by the same goal-scaled heat level as the
// Monthly heatmap, with the weekday letter centered and a marker on today.
void drawWeekHeatCell(GfxRenderer& renderer, const Rect& rect, const uint64_t readingMs, const char* letter,
                      const bool isToday) {
  const int level = getHeatLevel(readingMs);
  const Rect fill{rect.x + 1, rect.y + 1, std::max(0, rect.width - 2), std::max(0, rect.height - 2)};
  bool textBlack = true;
  switch (level) {
    case 1:
    case 2:
      renderer.fillRectDither(fill.x, fill.y, fill.width, fill.height, Color::LightGray);
      break;
    case 3:
    case 4:
      renderer.fillRectDither(fill.x, fill.y, fill.width, fill.height, Color::DarkGray);
      textBlack = (level < 4);
      break;
    case 5:
      renderer.fillRect(fill.x, fill.y, fill.width, fill.height);
      textBlack = false;
      break;
    default:
      break;
  }
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  const int letterW = renderer.getTextWidth(UI_12_FONT_ID, letter, EpdFontFamily::BOLD);
  const int letterH = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawText(UI_12_FONT_ID, rect.x + (rect.width - letterW) / 2, rect.y + (rect.height - letterH) / 2, letter,
                    textBlack, EpdFontFamily::BOLD);
  if (isToday) {
    renderer.drawRect(rect.x + 2, rect.y + 2, rect.width - 4, rect.height - 4, level >= 4 ? false : true);
  }
}

// Mon-Sun goal cells for the current week. Sized like the Monthly heatmap cells
// (square, full width) and shaded by the same goal-scaled heat level. Draws a
// row starting at (x, y); returns the cell (row) height, or 0 if the clock
// isn't set so the week can't be anchored.
int drawGoalWeekRow(GfxRenderer& renderer, int x, int y, int availWidth) {
  const uint32_t todayOrd = TimeUtils::getLocalDayOrdinal(READING_STATS.getDisplayTimestamp());
  int ty = 0;
  unsigned tm = 0;
  unsigned td = 0;
  if (!TimeUtils::getDateFromDayOrdinal(todayOrd, ty, tm, td)) {
    return 0;  // clock not set -> can't anchor the week
  }
  const int dow = statsDayOfWeek(ty, tm, td);  // 0 = Sunday
  const uint32_t mondayOrd = todayOrd - static_cast<uint32_t>((dow + 6) % 7);
  static constexpr char WEEK_LETTERS[7] = {'M', 'T', 'W', 'T', 'F', 'S', 'S'};
  constexpr int CELL_GAP = 6;  // matches Monthly's HEATMAP_GRID_GAP
  const int cell = (availWidth - CELL_GAP * 6) / 7;
  for (int d = 0; d < 7; ++d) {
    const uint32_t ord = mondayOrd + static_cast<uint32_t>(d);
    uint64_t ms = 0;
    for (const auto& rd : READING_STATS.getReadingDays()) {
      if (rd.dayOrdinal == ord) {
        ms = rd.readingMs;
        break;
      }
    }
    const int cx = x + d * (cell + CELL_GAP);
    const char letter[2] = {WEEK_LETTERS[d], '\0'};
    drawWeekHeatCell(renderer, Rect{cx, y, cell, cell}, ms, letter, ord == todayOrd);
  }
  return cell;
}

void drawLyraStyleButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                              const char* btn4) {
#if FREEINK_DEVICE_X4PRO
  // No front buttons to label on the X4 Pro. This screen paints its own hint
  // bar instead of going through the theme, so it needs the same treatment as
  // BaseTheme/LyraTheme::drawButtonHints: in Full Touch the labels become the
  // tappable action bar, in gesture mode the strip is 0-height and nothing is
  // drawn. This whole activity runs force-portrait (see onEnter), so the bar's
  // logical frame matches the frame taps arrive in.
  // Two slots: this screen's btn3/btn4 are Up/Down, which belong to the side
  // keys rather than the bar. See ActionBar.h on when a screen passes four.
  ActionBar::draw(renderer, UITheme::getInstance().getMetrics().buttonHintsHeight, SMALL_FONT_ID, /*rounded=*/true,
                  btn1, btn2, "", "");
  return;
#endif
  const GfxRenderer::Orientation originalOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 80;
  constexpr int smallButtonHeight = 15;
  constexpr int buttonHeight = 40;
  constexpr int buttonY = 40;
  constexpr int textYOffset = 7;
  constexpr int x4ButtonPositions[] = {58, 146, 254, 342};
  constexpr int x3ButtonPositions[] = {65, 157, 291, 383};
  const int* buttonPositions = gpio.deviceIsX3() ? x3ButtonPositions : x4ButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; ++i) {
    const int x = buttonPositions[i];
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      renderer.fillRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, SELECTION_RADIUS, Color::White);
      renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, SELECTION_RADIUS, true, true,
                               false, false, true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(SMALL_FONT_ID, textX, pageHeight - buttonY + textYOffset, labels[i]);
    } else {
      renderer.fillRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, SELECTION_RADIUS,
                               Color::White);
      renderer.drawRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, 1, SELECTION_RADIUS,
                               true, true, false, false, true);
    }
  }

  renderer.setOrientation(originalOrientation);
}

void civilFromDays(int z, int& year, unsigned& month, unsigned& day) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  year = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp + (mp < 10 ? 3 : -9);
  year += (month <= 2);
}

std::string formatMinutesLabel(const uint64_t readingMs) {
  const uint64_t totalMinutes = readingMs / 60000ULL;
  if (totalMinutes == 0) {
    return "";
  }
  return std::to_string(totalMinutes) + "m";
}

std::string formatDayLabel(const uint32_t dayOrdinal) {
  if (dayOrdinal == 0) {
    return "";
  }

  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civilFromDays(static_cast<int>(dayOrdinal), year, month, day);

  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%02u/%02u", day, month);
  return buffer;
}

std::string formatMonthLabel(const unsigned month) {
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "%02u", month);
  return buffer;
}

uint32_t getDisplayReferenceDayOrdinal() {
  const uint32_t displayTimestamp = READING_STATS.getDisplayTimestamp();
  if (!TimeUtils::isClockValid(displayTimestamp)) {
    return 0;
  }
  return TimeUtils::getLocalDayOrdinal(displayTimestamp);
}

std::vector<ChartBar> getRecentDailyReadingBars() {
  std::vector<ChartBar> bars(7);
  const auto& readingDays = READING_STATS.getReadingDays();
  if (readingDays.empty()) {
    return bars;
  }

  uint32_t referenceDayOrdinal = getDisplayReferenceDayOrdinal();
  if (referenceDayOrdinal == 0) {
    referenceDayOrdinal = readingDays.back().dayOrdinal;
  }

  for (int index = 0; index < 7; ++index) {
    const uint32_t dayOrdinal = referenceDayOrdinal >= static_cast<uint32_t>(6 - index)
                                    ? referenceDayOrdinal - static_cast<uint32_t>(6 - index)
                                    : 0;
    bars[index].bottomLabel = formatDayLabel(dayOrdinal);
    for (const auto& day : readingDays) {
      if (day.dayOrdinal == dayOrdinal) {
        bars[index].readingMs = day.readingMs;
        bars[index].topLabel = formatMinutesLabel(day.readingMs);
        break;
      }
    }
  }

  return bars;
}

int resolveReferenceYear(const std::vector<ReadingDayStats>& readingDays) {
  uint32_t referenceDayOrdinal = getDisplayReferenceDayOrdinal();
  if (referenceDayOrdinal == 0 && !readingDays.empty()) {
    referenceDayOrdinal = readingDays.back().dayOrdinal;
  }

  if (referenceDayOrdinal == 0) {
    return 0;
  }

  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civilFromDays(static_cast<int>(referenceDayOrdinal), year, month, day);
  return year;
}

std::vector<ChartBar> getAnnualReadingBars(int& year) {
  std::vector<ChartBar> bars(12);
  for (unsigned month = 1; month <= 12; ++month) {
    bars[month - 1].bottomLabel = formatMonthLabel(month);
  }

  const auto& readingDays = READING_STATS.getReadingDays();
  year = resolveReferenceYear(readingDays);
  if (year == 0) {
    return bars;
  }

  for (const auto& day : readingDays) {
    int dayYear = 0;
    unsigned dayMonth = 0;
    unsigned dayNumber = 0;
    civilFromDays(static_cast<int>(day.dayOrdinal), dayYear, dayMonth, dayNumber);
    if (dayYear != year || dayMonth == 0 || dayMonth > 12) {
      continue;
    }
    bars[dayMonth - 1].readingMs += day.readingMs;
  }

  return bars;
}

std::string formatAnnualReadingTitle(const int year) {
  if (year <= 0) {
    return tr(STR_ANNUAL_READING);
  }
  return std::string(tr(STR_ANNUAL_READING)) + " (" + std::to_string(year) + ")";
}

uint64_t getCurrentYearReadingMs() {
  const auto& readingDays = READING_STATS.getReadingDays();
  const int referenceYear = resolveReferenceYear(readingDays);
  if (referenceYear == 0) {
    return 0;
  }

  uint64_t totalMs = 0;
  for (const auto& day : readingDays) {
    int dayYear = 0;
    unsigned dayMonth = 0;
    unsigned dayNumber = 0;
    civilFromDays(static_cast<int>(day.dayOrdinal), dayYear, dayMonth, dayNumber);
    if (dayYear == referenceYear) {
      totalMs += day.readingMs;
    }
  }
  return totalMs;
}

void drawReadingChart(GfxRenderer& renderer, const Rect& rect, const std::vector<ChartBar>& bars,
                      const bool rotateBottomLabels) {
  if (bars.empty()) {
    return;
  }

  const int innerLeft = rect.x + 14;
  const int innerRight = rect.x + rect.width - 14;
  const int topLabelY = rect.y + 2;
  const int chartTop = rect.y + 30;
  const int bottomGap = rotateBottomLabels ? 12 : 10;
  const int bottomLabelAreaHeight = rotateBottomLabels ? 40 : 18;
  const int baselineY = rect.y + rect.height - bottomLabelAreaHeight - bottomGap - 2;
  const int bottomLabelY = baselineY + bottomGap;
  const int chartHeight = std::max(1, baselineY - chartTop);

  const int barCount = static_cast<int>(bars.size());
  const int barGap = barCount <= 7 ? 7 : 4;
  const int minBarWidth = barCount <= 7 ? 12 : 8;
  const int barWidth = std::max(minBarWidth, (innerRight - innerLeft - barGap * (barCount - 1)) / barCount);
  const int usedWidth = barWidth * barCount + barGap * (barCount - 1);
  const int chartLeft = rect.x + (rect.width - usedWidth) / 2;
  uint64_t maxValue = 1;
  for (const auto& bar : bars) {
    maxValue = std::max(maxValue, bar.readingMs);
  }

  renderer.drawLine(innerLeft - 2, baselineY, innerRight + 2, baselineY, 2, true);

  for (int index = 0; index < barCount; ++index) {
    const int barX = chartLeft + index * (barWidth + barGap);
    const uint64_t readingMs = bars[index].readingMs;
    if (!bars[index].topLabel.empty()) {
      const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, bars[index].topLabel.c_str(), EpdFontFamily::REGULAR);
      renderer.drawText(SMALL_FONT_ID, barX + (barWidth - labelWidth) / 2, topLabelY, bars[index].topLabel.c_str());
    }

    int barHeight = static_cast<int>((readingMs * chartHeight) / maxValue);
    if (readingMs > 0 && barHeight < 6) {
      barHeight = 6;
    }

    const int barY = baselineY - barHeight;
    if (barHeight > 0) {
      renderer.fillRectDither(barX + 1, barY + 1, std::max(0, barWidth - 2), std::max(0, barHeight - 2),
                              Color::LightGray);
      renderer.drawRect(barX, barY, barWidth, barHeight);
    } else {
      renderer.drawLine(barX, baselineY - 1, barX + barWidth, baselineY - 1);
    }

    if (bars[index].bottomLabel.empty()) {
      continue;
    }

    if (rotateBottomLabels) {
      const int labelWidth =
          renderer.getTextWidth(SMALL_FONT_ID, bars[index].bottomLabel.c_str(), EpdFontFamily::REGULAR);
      const int rotatedX = barX + (barWidth - renderer.getTextHeight(SMALL_FONT_ID)) / 2;
      const int rotatedY = bottomLabelY + (bottomLabelAreaHeight + labelWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, rotatedX, rotatedY, bars[index].bottomLabel.c_str());
    } else {
      const int labelWidth =
          renderer.getTextWidth(SMALL_FONT_ID, bars[index].bottomLabel.c_str(), EpdFontFamily::REGULAR);
      renderer.drawText(SMALL_FONT_ID, barX + (barWidth - labelWidth) / 2, bottomLabelY + 2,
                        bars[index].bottomLabel.c_str());
    }
  }
}

bool isLeapYear(const int year) { return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0); }

unsigned getDaysInMonth(const int year, const unsigned month) {
  static constexpr unsigned DAYS_PER_MONTH[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2) {
    return isLeapYear(year) ? 29U : 28U;
  }
  if (month < 1 || month > 12) {
    return 30;
  }
  return DAYS_PER_MONTH[month - 1];
}

void resolveReferenceMonth(int& year, unsigned& month) {
  const uint32_t referenceDayOrdinal = getDisplayReferenceDayOrdinal();
  unsigned day = 0;
  if (referenceDayOrdinal != 0 && TimeUtils::getDateFromDayOrdinal(referenceDayOrdinal, year, month, day)) {
    return;
  }
  if (READING_STATS.hasReadingDays() &&
      TimeUtils::getDateFromDayOrdinal(READING_STATS.getReadingDays().back().dayOrdinal, year, month, day)) {
    return;
  }
  year = 2026;
  month = 1;
}

int getHeatLevel(const uint64_t readingMs) {
  if (readingMs == 0) {
    return 0;
  }

  const uint64_t goalMs = getDailyReadingGoalMs();
  const uint64_t level1Ms = (goalMs + 1ULL) / 2ULL;
  const uint64_t level2Ms = goalMs;
  const uint64_t level3Ms = goalMs * 2ULL;
  const uint64_t level4Ms = goalMs * 4ULL;
  const uint64_t level5Ms = goalMs * 8ULL;

  if (readingMs < level1Ms) {
    return 0;
  }
  if (readingMs < level2Ms) {
    return 1;
  }
  if (readingMs < level3Ms) {
    return 2;
  }
  if (readingMs < level4Ms) {
    return 3;
  }
  if (readingMs < level5Ms) {
    return 4;
  }
  return 5;
}

uint32_t toHeatmapMinutes(const uint64_t readingMs) { return static_cast<uint32_t>((readingMs + 59999ULL) / 60000ULL); }

void formatHeatmapLabel(const uint64_t readingMs, char* buffer, const size_t bufferSize) {
  snprintf(buffer, bufferSize, "%um+", toHeatmapMinutes(readingMs));
}

MonthSummary buildMonthSummary(const int year, const unsigned month) {
  MonthSummary summary;
  const uint32_t monthStart = TimeUtils::getDayOrdinalForDate(year, month, 1);
  const uint32_t monthEnd = TimeUtils::getDayOrdinalForDate(year, month, getDaysInMonth(year, month));

  for (const auto& day : READING_STATS.getReadingDays()) {
    int dayYear = 0;
    unsigned dayMonth = 0;
    unsigned dayOfMonth = 0;
    if (!TimeUtils::getDateFromDayOrdinal(day.dayOrdinal, dayYear, dayMonth, dayOfMonth)) {
      continue;
    }

    if (dayYear == year) {
      summary.yearTotalReadingMs += day.readingMs;
    }

    if (day.dayOrdinal < monthStart || day.dayOrdinal > monthEnd) {
      continue;
    }

    summary.monthTotalReadingMs += day.readingMs;
    if (day.readingMs > 0) {
      summary.monthDaysRead++;
    }
    if (day.readingMs > summary.bestDayReadingMs) {
      summary.bestDayReadingMs = day.readingMs;
      summary.bestDayOfMonth = dayOfMonth;
    }
  }

  return summary;
}

std::array<HeatmapCell, 42> buildHeatmapCells(const int year, const unsigned month,
                                              const uint32_t referenceDayOrdinal) {
  std::array<HeatmapCell, 42> cells{};
  const uint32_t firstDayOrdinal = TimeUtils::getDayOrdinalForDate(year, month, 1);
  const int firstWeekday = static_cast<int>((firstDayOrdinal + 3U) % 7U);  // Monday = 0
  const uint32_t gridStartOrdinal = firstDayOrdinal - static_cast<uint32_t>(firstWeekday);

  for (size_t index = 0; index < cells.size(); ++index) {
    auto& cell = cells[index];
    cell.dayOrdinal = gridStartOrdinal + static_cast<uint32_t>(index);
    int cellYear = 0;
    unsigned cellMonth = 0;
    unsigned cellDay = 0;
    TimeUtils::getDateFromDayOrdinal(cell.dayOrdinal, cellYear, cellMonth, cellDay);
    cell.day = cellDay;
    cell.inViewedMonth = cellYear == year && cellMonth == month;
    cell.isReferenceDay = cell.inViewedMonth && referenceDayOrdinal != 0 && cell.dayOrdinal == referenceDayOrdinal;
  }

  size_t readingIndex = 0;
  const auto& readingDays = READING_STATS.getReadingDays();
  for (auto& cell : cells) {
    while (readingIndex < readingDays.size() && readingDays[readingIndex].dayOrdinal < cell.dayOrdinal) {
      readingIndex++;
    }
    if (readingIndex < readingDays.size() && readingDays[readingIndex].dayOrdinal == cell.dayOrdinal) {
      cell.readingMs = readingDays[readingIndex].readingMs;
    }
  }

  return cells;
}

void drawHeatCell(GfxRenderer& renderer, const Rect& rect, const HeatmapCell& cell) {
  const int level = cell.inViewedMonth ? getHeatLevel(cell.readingMs) : 0;
  const Rect fillRect{rect.x + 1, rect.y + 1, std::max(0, rect.width - 2), std::max(0, rect.height - 2)};
  bool textBlack = true;

  switch (level) {
    case 1:
    case 2:
      renderer.fillRectDither(fillRect.x, fillRect.y, fillRect.width, fillRect.height, Color::LightGray);
      break;
    case 3:
    case 4:
      renderer.fillRectDither(fillRect.x, fillRect.y, fillRect.width, fillRect.height, Color::DarkGray);
      textBlack = (level < 4);
      break;
    case 5:
      renderer.fillRect(fillRect.x, fillRect.y, fillRect.width, fillRect.height);
      textBlack = false;
      break;
    default:
      break;
  }

  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  if (cell.day > 0) {
    const std::string dayText = std::to_string(cell.day);
    renderer.drawText(SMALL_FONT_ID, rect.x + 6, rect.y + 5, dayText.c_str(), textBlack,
                      cell.inViewedMonth ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }

  if (cell.isReferenceDay) {
    renderer.drawRect(rect.x + 2, rect.y + 2, rect.width - 4, rect.height - 4, level >= 4 ? false : true);
  }
}

void drawLegendSwatch(GfxRenderer& renderer, const Rect& rect, const int level) {
  const Rect heatRect{rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2};
  switch (level) {
    case 1:
    case 2:
      renderer.fillRectDither(heatRect.x, heatRect.y, heatRect.width, heatRect.height, Color::LightGray);
      break;
    case 3:
    case 4:
      renderer.fillRectDither(heatRect.x, heatRect.y, heatRect.width, heatRect.height, Color::DarkGray);
      break;
    case 5:
      renderer.fillRect(heatRect.x, heatRect.y, heatRect.width, heatRect.height);
      break;
    default:
      break;
  }
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
}

void drawLegend(GfxRenderer& renderer, const Rect& rect) {
  struct LegendLevel {
    int level;
    char label[12];
  };
  const uint64_t goalMs = getDailyReadingGoalMs();
  static LegendLevel LEVELS[5];
  formatHeatmapLabel((goalMs + 1ULL) / 2ULL, LEVELS[0].label, sizeof(LEVELS[0].label));
  formatHeatmapLabel(goalMs, LEVELS[1].label, sizeof(LEVELS[1].label));
  formatHeatmapLabel(goalMs * 2ULL, LEVELS[2].label, sizeof(LEVELS[2].label));
  formatHeatmapLabel(goalMs * 4ULL, LEVELS[3].label, sizeof(LEVELS[3].label));
  formatHeatmapLabel(goalMs * 8ULL, LEVELS[4].label, sizeof(LEVELS[4].label));
  LEVELS[0].level = 1;
  LEVELS[1].level = 2;
  LEVELS[2].level = 3;
  LEVELS[3].level = 4;
  LEVELS[4].level = 5;
  constexpr int LEVEL_COUNT = sizeof(LEVELS) / sizeof(LEVELS[0]);

  const int itemWidth = rect.width / LEVEL_COUNT;
  for (int index = 0; index < LEVEL_COUNT; ++index) {
    const int itemX = rect.x + index * itemWidth;
    const Rect swatch{itemX + 6, rect.y + 3, LEGEND_SWATCH_SIZE, LEGEND_SWATCH_SIZE};
    drawLegendSwatch(renderer, swatch, LEVELS[index].level);
    renderer.drawText(SMALL_FONT_ID, itemX + 28, rect.y + 6, LEVELS[index].label);
  }
}
}  // namespace

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  captureFirstStatsAccessDate();

  // Force portrait so the stats layout is consistent regardless of the caller's
  // orientation (e.g. a landscape reader). Restored on exit.
  entryOrientation = renderer.getOrientation();
  if (entryOrientation != GfxRenderer::Orientation::Portrait) {
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
    restoreOrientationOnExit = true;
    fullRefreshNext = true;  // orientation flips must full-refresh to avoid ghosting
  }

  currentPage = PAGE_OVERVIEW;
  selectedItemIndex = 0;
  resolveReferenceMonth(viewedYear, viewedMonth);

  waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  waitForBackRelease = false;
  requestUpdate();
}

void ReadingStatsActivity::onExit() {
  if (restoreOrientationOnExit) {
    renderer.setOrientation(entryOrientation);
    restoreOrientationOnExit = false;
  }
  Activity::onExit();
}

void ReadingStatsActivity::changePage(const int delta) {
  currentPage += delta;
  while (currentPage < 0) {
    currentPage += TOTAL_STATS_PAGES;
  }
  while (currentPage >= TOTAL_STATS_PAGES) {
    currentPage -= TOTAL_STATS_PAGES;
  }
  // Confirm-on-ribbon advanced the tab; stay on the ribbon so successive
  // confirms keep cycling tabs (matches SettingsActivity::loop's path).
  selectedItemIndex = 0;
  requestUpdate();
}

void ReadingStatsActivity::changeViewedMonth(const int delta) {
  int month = static_cast<int>(viewedMonth) + delta;
  int year = viewedYear;
  while (month < 1) {
    month += 12;
    year--;
  }
  while (month > 12) {
    month -= 12;
    year++;
  }
  viewedYear = year;
  viewedMonth = static_cast<unsigned>(month);
  requestUpdate();
}

void ReadingStatsActivity::loop() {
  if (waitForBackRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Back) &&
        !mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      waitForBackRelease = false;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Closes from any tab, in every mode. Full Touch used to step to the
    // previous tab here and close only from the first; that made leaving take
    // as many gestures as there are tabs, and the ribbon is directly tappable
    // anyway. See the press handler below for why Full Touch acts on release.
    finish();
    return;
  }

  if (waitForConfirmRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      waitForConfirmRelease = false;
    }
    return;
  }

#if FREEINK_DEVICE_X4PRO
  // Full Touch: a tap on a tab selects that category directly; on the Books
  // and Sessions tabs a tap on a row selects it, and a second tap on the
  // selected row opens it. Both hit-tests need the raw point, so this reads
  // wasTapPoint directly instead of the TouchListNav helper (mirrors
  // SettingsActivity).
  if (SETTINGS.fullTouchUi) {
    // Swipe right = next tab, wrapping like Confirm-on-the-ribbon does. The
    // leftward swipe is Back and walks back out through the tabs (above).
    if (TouchListNav::tabSwipeNext(mappedInput)) {
      changePage(1);
      scrollOffset = 0;  // each tab always opens at the top
      return;
    }
    int lx, ly;
    // Long tap on a Books-tab row = the hold-Confirm "remove stats entry" for
    // that row. Suppress the contact so the lift doesn't also tap.
    if (currentPage == PAGE_STARTED_BOOKS && mappedInput.wasTouchLongPressPoint(lx, ly)) {
      const int rowIndex = GUI.hitTestList(listRect(), currentPageItemCount(), selectedItemIndex - 1, true, lx, ly);
      if (rowIndex >= 0) {
        selectedItemIndex = rowIndex + 1;
        mappedInput.suppressTouchContact();
        confirmRemoveSelectedBook();
        return;
      }
    }
    if (mappedInput.wasTapPoint(lx, ly)) {
      std::vector<TabInfo> tabs;
      buildTabs(tabs);
      const int tabIndex = GUI.hitTestTabBar(renderer, tabBarRect(), tabs, lx, ly);
      if (tabIndex >= 0) {
        currentPage = tabIndex;
        scrollOffset = 0;       // each tab always opens at the top
        selectedItemIndex = 0;  // focus moves to the tab bar, as in the button flow
        requestUpdate();
        return;
      }
      if (currentPage == PAGE_STARTED_BOOKS || currentPage == PAGE_SESSIONS) {
        // selectedItemIndex - 1 mirrors what render() passes to drawList, so
        // the hit-test sees the same visible page (-1 = ribbon focused, no
        // row highlighted).
        const int rowIndex = GUI.hitTestList(listRect(), currentPageItemCount(), selectedItemIndex - 1, true, lx, ly);
        if (rowIndex >= 0) {
          if (rowIndex + 1 != selectedItemIndex) {
            selectedItemIndex = rowIndex + 1;
            requestUpdate();
          } else if (currentPage == PAGE_STARTED_BOOKS) {
            openSelectedBook();
          } else {
            openSelectedSessionEditor();
          }
          return;
        }
      }
    }
  }
#endif

  // Mirrors SettingsActivity's controls so the two tabbed screens behave
  // identically. The "ribbon" is selectedItemIndex == 0 (the tab bar at
  // the top); positions 1..N are the content rows on the current page.
  //   Confirm on ribbon         → advance to next tab
  //   Confirm on item           → page-specific action (open book, edit session)
  //   Back on item              → return to ribbon
  //   Back on ribbon            → exit Stats
  //   short Up/Down/Left/Right  → cycle selection through {ribbon + items}
  //   long  Up/Down/Left/Right  → cycle to adjacent tab directly
  // Monthly is the one special case: it has zero content items, so Up/Down
  // is repurposed to step the viewed month while selectedItemIndex stays 0.
  bool hasChangedPage = false;

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedItemIndex == 0) {
      changePage(1);
      return;
    }
    if (currentPage == PAGE_STARTED_BOOKS) {
      if (mappedInput.getHeldTime() >= BOOK_LONG_PRESS_MS) {
        confirmRemoveSelectedBook();
      } else {
        openSelectedBook();
      }
      return;
    }
    if (currentPage == PAGE_SESSIONS) {
      openSelectedSessionEditor();
      return;
    }
    // Pages without per-item actions (Overview/Weekly/Monthly): no-op on
    // content positions. selectedItemIndex stays at 0 on those pages anyway
    // because currentPageItemCount() returns 0.
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
#if FREEINK_DEVICE_X4PRO
    // Full Touch handles Back on the RELEASE frame (top of loop). Both an
    // injected swipe and an action-bar tap press on one frame and release on
    // the next, so this press frame runs first and must do nothing at all —
    // otherwise one Back would do two things: reset row focus here, then close
    // on the release. The row→ribbon step is not part of the way out in Full
    // Touch anyway (taps move the cursor; there is nothing to back out of).
    if (SETTINGS.fullTouchUi) {
      return;
    }
#endif
    if (selectedItemIndex > 0) {
      selectedItemIndex = 0;
      requestUpdate();
    } else {
      finish();
    }
    return;
  }

  // Monthly's month-stepper hijacks short Up/Down. Doing this before the
  // generic navigator keeps the controls familiar: pressing Up/Down on the
  // Monthly heatmap steps months even when nothing is "selected" inside it.
  if (currentPage == PAGE_MONTHLY && selectedItemIndex == 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      changeViewedMonth(-1);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      changeViewedMonth(1);
      return;
    }
  }

  // Overview and Weekly scroll vertically when their content overflows. Short
  // Up/Down OR Left/Right step the scroll offset; long-press still cycles tabs
  // via the navigator below. maxScroll is set during render.
  const bool scrollablePage = (currentPage == PAGE_OVERVIEW || currentPage == PAGE_WEEKLY);
  if (scrollablePage && selectedItemIndex == 0 && maxScroll > 0) {
    // Full Touch: a vertical swipe scrolls a screenful, in the same content-drag
    // sense as the paginated lists (swipe up reveals what is below). Without
    // this the swipe is dead here — Full Touch records up/down instead of
    // injecting them as the Up/Down presses the branch below reads, so a page
    // with a visible scroll bar could only be moved with the side keys.
    // maxScroll > 0 above is the bounds check pageSwipeDelta requires.
    switch (TouchListNav::pageSwipeDelta(mappedInput)) {
      case +1:
        scrollOffset = std::min(scrollOffset + STATS_SCROLL_PAGE, maxScroll);
        requestUpdate();
        return;
      case -1:
        scrollOffset = std::max(scrollOffset - STATS_SCROLL_PAGE, 0);
        requestUpdate();
        return;
      default:
        break;
    }

    const bool scrollDown = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);
    const bool scrollUp = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                          mappedInput.wasReleased(MappedInputManager::Button::Left);
    if (scrollDown) {
      scrollOffset = std::min(scrollOffset + STATS_SCROLL_STEP, maxScroll);
      requestUpdate();
      return;
    }
    if (scrollUp) {
      scrollOffset = std::max(scrollOffset - STATS_SCROLL_STEP, 0);
      requestUpdate();
      return;
    }
  }

  const int pageItemCount = currentPageItemCount();
  const int navTotal = pageItemCount + 1;  // +1 for the ribbon

  buttonNavigator.onNextRelease([this, navTotal] {
    selectedItemIndex = ButtonNavigator::nextIndex(selectedItemIndex, navTotal);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, navTotal] {
    selectedItemIndex = ButtonNavigator::previousIndex(selectedItemIndex, navTotal);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, &hasChangedPage] {
    hasChangedPage = true;
    currentPage = ButtonNavigator::nextIndex(currentPage, TOTAL_STATS_PAGES);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, &hasChangedPage] {
    hasChangedPage = true;
    currentPage = ButtonNavigator::previousIndex(currentPage, TOTAL_STATS_PAGES);
    requestUpdate();
  });

  if (hasChangedPage) {
    // Reset scroll so each tab always opens at the top.
    scrollOffset = 0;
    // Same rule as SettingsActivity: keep the ribbon focus on the ribbon, or
    // hop to the first content row of the new tab if the user was already in
    // the list (clamped to the new page's actual count).
    const int newItemCount = currentPageItemCount();
    if (selectedItemIndex == 0 || newItemCount == 0) {
      selectedItemIndex = 0;
    } else {
      selectedItemIndex = 1;
    }
  }
}

int ReadingStatsActivity::currentPageItemCount() const {
  switch (currentPage) {
    case PAGE_STARTED_BOOKS: {
      const int totalBooks = static_cast<int>(getUnfinishedBooks().size());
      return std::min(totalBooks, BOOKS_PER_PAGE);
    }
    case PAGE_SESSIONS: {
      const int undated = static_cast<int>(collectUndatedSessionIndices().size());
      return std::min(undated, SESSIONS_PER_PAGE);
    }
    default:
      return 0;
  }
}

void ReadingStatsActivity::openSelectedBook() {
  const auto books = getUnfinishedBooks();
  // selectedItemIndex == 0 is the ribbon; content rows start at 1.
  const int bookRow = selectedItemIndex - 1;
  if (bookRow < 0 || bookRow >= static_cast<int>(books.size())) {
    return;
  }

  startActivityForResult(std::make_unique<ReadingStatsDetailActivity>(renderer, mappedInput, books[bookRow]->path),
                         [this](const ActivityResult&) {
                           guardBackReturn();
                           requestUpdate();
                         });
}

void ReadingStatsActivity::openSelectedSessionEditor() {
  // The Sessions tab only shows undated entries. Resolve the display row to a
  // real sessionLog index via the same helper used at render time.
  const auto undated = collectUndatedSessionIndices();
  const int sessionCount = std::min(static_cast<int>(undated.size()), SESSIONS_PER_PAGE);
  const int sessionRow = selectedItemIndex - 1;
  if (sessionCount <= 0 || sessionRow < 0 || sessionRow >= sessionCount) {
    return;
  }
  const size_t logIndex = undated[static_cast<size_t>(sessionRow)];
  startActivityForResult(std::make_unique<SessionDateEditActivity>(renderer, mappedInput, logIndex),
                         [this](const ActivityResult&) {
                           guardBackReturn();
                           requestUpdate();
                         });
}

void ReadingStatsActivity::confirmRemoveSelectedBook() {
  const auto books = getUnfinishedBooks();
  const int bookRow = selectedItemIndex - 1;
  if (bookRow < 0 || bookRow >= static_cast<int>(books.size())) {
    return;
  }

  const ReadingBookStats selectedBook = *books[bookRow];
  const int currentSelection = bookRow;
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_STATS_ENTRY),
                                                                getBookTitle(selectedBook)),
                         [this, selectedBook, currentSelection](const ActivityResult& result) {
                           if (!result.isCancelled && READING_STATS.removeBook(selectedBook.path)) {
                             const int bookCount =
                                 std::min(static_cast<int>(getUnfinishedBooks().size()), BOOKS_PER_PAGE);
                             if (bookCount == 0) {
                               selectedItemIndex = 0;
                             } else {
                               // Keep cursor on a valid content row (clamped to the new count).
                               const int newRow = std::min(currentSelection, bookCount - 1);
                               selectedItemIndex = newRow + 1;
                             }
                           }

                           guardBackReturn();
                           requestUpdate(true);
                         });
}

void ReadingStatsActivity::guardBackReturn() { waitForBackRelease = true; }

Rect ReadingStatsActivity::tabBarRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, metrics.topPadding + metrics.headerHeight, renderer.getScreenWidth(), metrics.tabBarHeight};
}

// List body of the two row-list tabs. Only meaningful on Books/Sessions;
// mirrors the exact layout math of those tabs' render branches.
Rect ReadingStatsActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - 4;
  int listTop;
  int rowsPerPage;
  if (currentPage == PAGE_SESSIONS) {
    const int bucketCardsTop = contentTop + LIST_HEADER_HEIGHT + 4;
    const int inboxTop = bucketCardsTop + BUCKET_CARD_HEIGHT + SECTION_GAP;
    listTop = inboxTop + LIST_HEADER_HEIGHT + LIST_HEADER_BOTTOM_GAP;
    rowsPerPage = SESSIONS_PER_PAGE;
  } else {  // PAGE_STARTED_BOOKS
    listTop = contentTop + LIST_HEADER_HEIGHT + LIST_HEADER_BOTTOM_GAP;
    rowsPerPage = BOOKS_PER_PAGE;
  }
  // The cap is rowsPerPage rows PLUS the page-counter strip drawList takes out
  // of the bottom of whatever rect it is given — without it the strip would
  // cost one of the four rows these tabs are supposed to show.
  const int listHeight =
      std::min(std::max(0, contentBottom - listTop), GUI.listRectHeightForRows(rowsPerPage, /*hasSubtitle=*/true));
  return Rect{0, listTop, renderer.getScreenWidth(), listHeight};
}

void ReadingStatsActivity::buildTabs(std::vector<TabInfo>& tabs) const {
  const bool hasUndatedSessions = !collectUndatedSessionIndices().empty();
  tabs.reserve(TOTAL_STATS_PAGES);
  for (int i = 0; i < TOTAL_STATS_PAGES; ++i) {
    tabs.push_back({I18N.get(TAB_NAMES[i]), currentPage == i, i == PAGE_SESSIONS && hasUndatedSessions});
  }
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int sidePadding = metrics.contentSidePadding;
  const int contentWidth = pageWidth - sidePadding * 2;
  // Tab bar lives directly below the header. Page content starts after the tab
  // bar and runs to contentBottom — buttonHints and a small gap.
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - 4;
  // The two SCROLLING tabs (Overview/Weekly) give their bottom strip to the
  // page counter, the same strip every list screen reserves — a scroll bar
  // alone is too easy to miss. The fixed tabs draw to contentBottom as before.
  const int scrollBottom = contentBottom - metrics.pageIndicatorHeight;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_STATS),
                 nullptr);

  std::vector<TabInfo> tabs;
  buildTabs(tabs);
  GUI.drawTabBar(renderer, tabBarRect(), tabs, selectedItemIndex == 0);

  // Reset each render; scrollable tabs (Overview/Weekly) set it from their
  // measured content height so loop() knows whether Up/Down/Left/Right scroll.
  maxScroll = 0;

  // Shared finish step for scrollable tabs: mask content that ran past the
  // viewport, repaint the pinned header/tab bar, and draw a right-edge
  // scrollbar. Call after drawing the page's scroll-offset content.
  auto drawScrollChrome = [&]() {
    renderer.fillRect(0, scrollBottom, pageWidth, pageHeight - scrollBottom, false);
    if (scrollOffset > 0) {
      renderer.fillRect(0, 0, pageWidth, contentTop, false);
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_STATS),
                     nullptr);
      GUI.drawTabBar(renderer, tabBarRect(), tabs, selectedItemIndex == 0);
    }
    if (maxScroll > 0) {
      const int trackX = pageWidth - 5;
      const int trackTop = contentTop;
      const int trackH = scrollBottom - contentTop;
      const int totalContent = trackH + maxScroll;
      const int thumbH = std::max(20, trackH * trackH / totalContent);
      const int thumbY = trackTop + (trackH - thumbH) * scrollOffset / maxScroll;
      renderer.drawLine(trackX + 1, trackTop, trackX + 1, trackTop + trackH);
      renderer.fillRect(trackX, thumbY, 3, thumbH, true);

      // Say in words what the thumb says in pixels. Continuous scroll, so a
      // "page" is one viewport-worth: the same unit Up/Down move by.
      if (trackH > 0) {
        const int totalPages = (totalContent + trackH - 1) / trackH;
        const int currentPageNo = std::min(totalPages, scrollOffset / trackH + 1);
        GUI.drawPageIndicator(renderer, Rect{0, contentTop, pageWidth, contentBottom - contentTop}, currentPageNo,
                              totalPages);
      }
    }
  };

  if (currentPage == PAGE_OVERVIEW) {
    const uint64_t todayReadingMs = READING_STATS.getTodayReadingMs();
    const std::string dailyGoalValue = ReadingStatsAnalytics::formatDurationHm(todayReadingMs) + " / " +
                                       ReadingStatsAnalytics::formatDurationHm(getDailyReadingGoalMs());
    const auto profile = ReadingStatsAnalytics::buildReadingProfileSummary();

    // The Overview content (stat rows + Reading Profile) can exceed a screenful,
    // so it scrolls. Everything is laid out in virtual coordinates from 0 and
    // drawn at (contentTop + virtualY - scroll); content that lands above/below
    // the viewport is masked after drawing. (The annual chart moved to Sessions.)
    const int viewportHeight = scrollBottom - contentTop;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    constexpr int PROFILE_NAME_HEIGHT = 22;
    constexpr int PROFILE_DIM_GAP = 8;

    // Pre-wrap the four dimension descriptions so we can measure total height.
    const StrId dimNameIds[4] = {StrId::STR_HABIT, StrId::STR_STABILITY, StrId::STR_ENGAGEMENT, StrId::STR_DEPTH};
    const StrId dimDescIds[4] = {StrId::STR_HABIT_DESC, StrId::STR_STABILITY_DESC, StrId::STR_ENGAGEMENT_DESC,
                                 StrId::STR_DEPTH_DESC};
    const int dimScores[4] = {profile.habit.score, profile.stability.score, profile.engagement.score,
                              profile.depth.score};
    std::array<std::vector<std::string>, 4> dimDescLines;
    std::array<std::vector<std::string>, 4> dimBreakLines;
    if (profile.hasData) {
      // Labelled per-dimension breakdown (the raw numbers behind each score).
      const std::string breaks[4] = {
          std::string(I18N.get(StrId::STR_DAYS_READ)) + " " + profile.habit.primaryValue + "   " +
              I18N.get(StrId::STR_GOALS_MET) + " " + profile.habit.secondaryValue,
          std::string(I18N.get(StrId::STR_READ_STREAK)) + " " + profile.stability.primaryValue + "   " +
              I18N.get(StrId::STR_BEST_DAY_SHARE) + " " + profile.stability.secondaryValue,
          std::string(I18N.get(StrId::STR_SESSIONS)) + " " + profile.engagement.primaryValue + "   " +
              I18N.get(StrId::STR_PER_READ_DAY) + " " + profile.engagement.secondaryValue,
          std::string(I18N.get(StrId::STR_SESSIONS_UNDER_10M)) + " " + profile.depth.primaryValue + "   " +
              I18N.get(StrId::STR_SESSIONS_10M_TO_29M) + " " + profile.depth.secondaryValue + "   " +
              I18N.get(StrId::STR_SESSIONS_30M_PLUS) + " " + profile.depth.tertiaryValue,
      };
      for (int i = 0; i < 4; ++i) {
        dimBreakLines[i] = renderer.wrappedText(UI_10_FONT_ID, breaks[i].c_str(), contentWidth, 2);
        dimDescLines[i] = renderer.wrappedText(UI_10_FONT_ID, I18N.get(dimDescIds[i]), contentWidth, 3);
      }
    }

    // ---- Measure pass: total virtual content height ----
    constexpr int SUMMARY_ROW_COUNT = 9;
    int totalHeight = SUMMARY_ROW_HEIGHT * SUMMARY_ROW_COUNT;
    if (profile.hasData) {
      totalHeight += SUMMARY_GAP + CHART_HEADER_HEIGHT + 4;  // Profile sub-header
      for (int i = 0; i < 4; ++i) {
        totalHeight += PROFILE_NAME_HEIGHT +
                       static_cast<int>(dimBreakLines[i].size() + dimDescLines[i].size()) * lineHeight +
                       PROFILE_DIM_GAP;
      }
    }
    totalHeight += SUMMARY_GAP;  // small bottom margin

    maxScroll = std::max(0, totalHeight - viewportHeight);
    scrollOffset = std::clamp(scrollOffset, 0, maxScroll);

    // ---- Draw pass ----
    const int dy = contentTop - scrollOffset;
    int y = dy;

    // "Sessions today" row also shows the lifetime average sessions per reading
    // day, e.g. "3  (2.1/day)". Guard against zero reading days.
    char sessionsValue[48];
    {
      const uint32_t sessionsToday = READING_STATS.getSessionsToday();
      const size_t daysRead = READING_STATS.getReadingDays().size();
      if (daysRead > 0) {
        const uint32_t perDayX10 = static_cast<uint32_t>(
            (static_cast<uint64_t>(READING_STATS.getTotalSessionCount()) * 10 + daysRead / 2) / daysRead);
        snprintf(sessionsValue, sizeof(sessionsValue), "%u  (%u.%u/day)", sessionsToday, perDayX10 / 10,
                 perDayX10 % 10);
      } else {
        snprintf(sessionsValue, sizeof(sessionsValue), "%u", sessionsToday);
      }
    }

    const uint8_t* rowIcons[SUMMARY_ROW_COUNT] = {Streak24Icon,      Confetti24Icon, Checkbox24Icon,
                                                  Readingtime24Icon, Check24Icon,    Files24Icon,
                                                  Readingtime24Icon, Award24Icon,    Receipttotal24Icon};
    const char* rowLabels[SUMMARY_ROW_COUNT] = {tr(STR_STREAK),       tr(STR_MAX_STREAK),      tr(STR_DAILY_GOAL),
                                                tr(STR_READING_TIME), tr(STR_BOOKS_FINISHED),  tr(STR_BOOKS_STARTED),
                                                tr(STR_AVG_SESSION),  tr(STR_LONGEST_SESSION), tr(STR_SESSIONS_TODAY)};
    const std::string rowValues[SUMMARY_ROW_COUNT] = {
        std::to_string(READING_STATS.getCurrentStreakDays()),
        std::to_string(READING_STATS.getMaxStreakDays()),
        dailyGoalValue,
        ReadingStatsAnalytics::formatDurationHm(READING_STATS.getTotalReadingMs()),
        std::to_string(READING_STATS.getBooksFinishedCount()),
        std::to_string(READING_STATS.getBooksStartedCount()),
        ReadingStatsAnalytics::formatDurationHm(READING_STATS.getAverageSessionMs()),
        ReadingStatsAnalytics::formatDurationHm(READING_STATS.getLongestSessionMs()),
        std::string(sessionsValue)};
    for (int i = 0; i < SUMMARY_ROW_COUNT; ++i) {
      drawMetricRow(renderer, Rect{sidePadding, y, contentWidth, SUMMARY_ROW_HEIGHT}, rowIcons[i], rowLabels[i],
                    rowValues[i]);
      y += SUMMARY_ROW_HEIGHT;
    }

    if (profile.hasData) {
      y += SUMMARY_GAP;
      GUI.drawSubHeader(renderer, Rect{0, y, pageWidth, CHART_HEADER_HEIGHT}, tr(STR_STATS_TAB_PROFILE),
                        std::to_string(profile.totalScore).c_str());
      y += CHART_HEADER_HEIGHT + 4;
      for (int i = 0; i < 4; ++i) {
        const std::string nameLine = std::string(I18N.get(dimNameIds[i])) + "   " + std::to_string(dimScores[i]);
        renderer.drawText(UI_10_FONT_ID, sidePadding, y, nameLine.c_str(), true, EpdFontFamily::BOLD);
        y += PROFILE_NAME_HEIGHT;
        for (const auto& breakLine : dimBreakLines[i]) {
          renderer.drawText(UI_10_FONT_ID, sidePadding, y, breakLine.c_str(), true, EpdFontFamily::BOLD);
          y += lineHeight;
        }
        for (const auto& descLine : dimDescLines[i]) {
          renderer.drawText(UI_10_FONT_ID, sidePadding, y, descLine.c_str());
          y += lineHeight;
        }
        y += PROFILE_DIM_GAP;
      }
    }

    drawScrollChrome();
  } else if (currentPage == PAGE_STARTED_BOOKS) {
    const auto books = getUnfinishedBooks();
    const int totalBooks = static_cast<int>(books.size());
    // Books tab is capped at one screenful — clamp both the visible-count and
    // the selection so we don't paginate within the tab. Users see the top
    // BOOKS_PER_PAGE titles; older ones are reachable via the per-book detail
    // screen (TODO if we ever need a longer list).
    const int bookCount = std::min(totalBooks, BOOKS_PER_PAGE);

    const std::string startedBooksLabel =
        std::string(tr(STR_BOOKS_STARTED)) + " (" + std::to_string(READING_STATS.getBooksStartedCount()) + ")";
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, LIST_HEADER_HEIGHT}, startedBooksLabel.c_str(), nullptr);

    const Rect contentRect = listRect();
    if (books.empty()) {
      renderer.drawText(UI_10_FONT_ID, sidePadding, contentRect.y + 20, tr(STR_NO_READING_STATS));
    } else {
      // -1 highlights no row, which is what we want when the ribbon is focused.
      const int highlightedRow = (selectedItemIndex > 0) ? (selectedItemIndex - 1) : -1;
      GUI.drawList(
          renderer, contentRect, bookCount, highlightedRow,
          [&](const int index) { return getBookTitle(*books[index]); },
          [&](const int index) {
            return getBookSubtitle(*books[index]) + " | " +
                   ReadingStatsAnalytics::formatDurationHm(books[index]->totalReadingMs);
          },
          nullptr, [&](const int index) { return std::to_string(books[index]->lastProgressPercent) + "%"; }, false);
    }
  } else if (currentPage == PAGE_WEEKLY) {
    const std::vector<ChartBar> weekBars = getRecentDailyReadingBars();
    const uint64_t last7DaysValueMs = READING_STATS.getRecentReadingMs(7);

    uint32_t daysRead = 0;
    uint32_t goalDays = 0;
    uint64_t bestDayMs = 0;
    std::string bestDayLabel = "-";
    for (const auto& bar : weekBars) {
      if (bar.readingMs > 0) {
        daysRead++;
      }
      if (bar.readingMs >= getDailyReadingGoalMs()) {
        goalDays++;
      }
      if (bar.readingMs > bestDayMs) {
        bestDayMs = bar.readingMs;
        bestDayLabel = bar.bottomLabel;
      }
    }

    // The Weekly content (large goal boxes + rows + chart) overflows a screen,
    // so it scrolls the same way Overview does: virtual coordinates from 0,
    // drawn at (contentTop + virtualY - scroll).
    const uint64_t avgDayMs = last7DaysValueMs / 7ULL;
    const std::string bestDayValue =
        (bestDayMs == 0) ? std::string("-")
                         : (bestDayLabel + " (" + ReadingStatsAnalytics::formatDurationHm(bestDayMs) + ")");
    const bool weekAnchored = TimeUtils::isClockValid(READING_STATS.getDisplayTimestamp());
    const int weekCell = (contentWidth - 36) / 7;  // square, Monthly-sized
    constexpr int WEEK_CHART_HEIGHT = 300;
    const int weekBlockHeight = weekAnchored ? weekCell + SUMMARY_GAP * 2 : 0;

    const int viewportHeight = scrollBottom - contentTop;
    const int totalHeight = weekBlockHeight + SUMMARY_ROW_HEIGHT * 5 + SUMMARY_GAP * 2 + CHART_HEADER_HEIGHT +
                            CHART_TOP_GAP + WEEK_CHART_HEIGHT;
    maxScroll = std::max(0, totalHeight - viewportHeight);
    scrollOffset = std::clamp(scrollOffset, 0, maxScroll);

    int y = contentTop - scrollOffset;
    if (weekAnchored) {
      drawGoalWeekRow(renderer, sidePadding, y, contentWidth);
      y += weekCell + SUMMARY_GAP * 2;
    }

    // Average daily reading over the last 7 days (labelled clearly so it isn't
    // mistaken for "today"). Last-30-days lives on the Monthly tab now.
    drawMetricRow(renderer, Rect{sidePadding, y, contentWidth, SUMMARY_ROW_HEIGHT}, Last7days24Icon, tr(STR_LAST_7D),
                  ReadingStatsAnalytics::formatDurationHm(last7DaysValueMs));
    drawMetricRow(renderer, Rect{sidePadding, y + SUMMARY_ROW_HEIGHT, contentWidth, SUMMARY_ROW_HEIGHT}, Book24Icon,
                  tr(STR_DAILY_AVERAGE), ReadingStatsAnalytics::formatDurationHm(avgDayMs));
    drawMetricRow(renderer, Rect{sidePadding, y + SUMMARY_ROW_HEIGHT * 2, contentWidth, SUMMARY_ROW_HEIGHT},
                  Check24Icon, tr(STR_DAYS_READ), std::to_string(daysRead));
    drawMetricRow(renderer, Rect{sidePadding, y + SUMMARY_ROW_HEIGHT * 3, contentWidth, SUMMARY_ROW_HEIGHT},
                  Checkbox24Icon, tr(STR_DAILY_GOAL), std::to_string(goalDays) + "/7");
    drawMetricRow(renderer, Rect{sidePadding, y + SUMMARY_ROW_HEIGHT * 4, contentWidth, SUMMARY_ROW_HEIGHT},
                  Award24Icon, tr(STR_BEST_DAY), bestDayValue);
    y += SUMMARY_ROW_HEIGHT * 5 + SUMMARY_GAP * 2;

    GUI.drawSubHeader(renderer, Rect{0, y, pageWidth, CHART_HEADER_HEIGHT}, tr(STR_DAILY_READING), nullptr);
    y += CHART_HEADER_HEIGHT + CHART_TOP_GAP;
    drawReadingChart(renderer, Rect{sidePadding, y, contentWidth, WEEK_CHART_HEIGHT}, weekBars, true);

    drawScrollChrome();
  } else if (currentPage == PAGE_MONTHLY) {
    const uint32_t referenceDayOrdinal = getDisplayReferenceDayOrdinal();
    const auto monthSummary = buildMonthSummary(viewedYear, viewedMonth);
    const auto cells = buildHeatmapCells(viewedYear, viewedMonth, referenceDayOrdinal);
    const std::string monthLabel = ReadingStatsAnalytics::formatMonthLabel(viewedYear, viewedMonth);

    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, MONTH_HEADER_HEIGHT}, monthLabel.c_str(), nullptr);

    const int summaryTop = contentTop + MONTH_HEADER_HEIGHT + 4;
    const std::string bestDayValue = monthSummary.bestDayOfMonth > 0
                                         ? ReadingStatsAnalytics::formatDurationHm(monthSummary.bestDayReadingMs) +
                                               " (" + std::to_string(monthSummary.bestDayOfMonth) + ")"
                                         : ReadingStatsAnalytics::formatDurationHm(monthSummary.bestDayReadingMs);

    drawMetricRow(renderer, Rect{sidePadding, summaryTop, contentWidth, SUMMARY_ROW_HEIGHT}, Receipttotal24Icon,
                  tr(STR_MONTH_TOTAL), ReadingStatsAnalytics::formatDurationHm(monthSummary.monthTotalReadingMs));
    drawMetricRow(renderer, Rect{sidePadding, summaryTop + SUMMARY_ROW_HEIGHT, contentWidth, SUMMARY_ROW_HEIGHT},
                  Check24Icon, tr(STR_DAYS_READ), std::to_string(monthSummary.monthDaysRead));
    drawMetricRow(renderer, Rect{sidePadding, summaryTop + SUMMARY_ROW_HEIGHT * 2, contentWidth, SUMMARY_ROW_HEIGHT},
                  Award24Icon, tr(STR_BEST_DAY), bestDayValue);

    const int gridTop = summaryTop + SUMMARY_ROW_HEIGHT * 3 + SECTION_GAP;
    const int legendTop = contentBottom - LEGEND_HEIGHT - 4;
    const int gridHeight = std::max(100, legendTop - gridTop - SECTION_GAP);
    const int cellWidth = (contentWidth - HEATMAP_GRID_GAP * 6) / 7;
    const int cellHeight = (gridHeight - HEATMAP_GRID_GAP * 5) / 6;

    for (int index = 0; index < 42; ++index) {
      const int row = index / 7;
      const int col = index % 7;
      const int x = sidePadding + col * (cellWidth + HEATMAP_GRID_GAP);
      const int y = gridTop + row * (cellHeight + HEATMAP_GRID_GAP);
      drawHeatCell(renderer, Rect{x, y, cellWidth, cellHeight}, cells[static_cast<size_t>(index)]);
    }

    drawLegend(renderer, Rect{sidePadding, legendTop, contentWidth, LEGEND_HEIGHT});
  } else if (currentPage == PAGE_SESSIONS) {
    // Session-length distribution over the recorded session log (bounded to the
    // most recent MAX_SESSION_LOG_ENTRIES sessions). Purely informational — the
    // cards are not selectable, so navigation is unchanged.
    const auto buckets = ReadingStatsAnalytics::countSessionDurationBuckets();
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, LIST_HEADER_HEIGHT}, tr(STR_SESSION_LENGTHS), nullptr);
    const int bucketCardsTop = contentTop + LIST_HEADER_HEIGHT + 4;
    constexpr int BUCKET_CARD_GAP = 8;
    const int bucketCardWidth = (contentWidth - BUCKET_CARD_GAP * 2) / 3;
    const StrId bucketLabels[3] = {StrId::STR_SESSIONS_UNDER_10M, StrId::STR_SESSIONS_10M_TO_29M,
                                   StrId::STR_SESSIONS_30M_PLUS};
    const uint32_t bucketValues[3] = {buckets.under10, buckets.mid, buckets.over30};
    for (int i = 0; i < 3; ++i) {
      const int cardX = sidePadding + i * (bucketCardWidth + BUCKET_CARD_GAP);
      AppMetricCard::Options bucketOpts;
      bucketOpts.labelY = 38;
      AppMetricCard::draw(renderer, Rect{cardX, bucketCardsTop, bucketCardWidth, BUCKET_CARD_HEIGHT},
                          I18N.get(bucketLabels[i]), std::to_string(bucketValues[i]), bucketOpts);
    }

    // The rest of the Sessions tab is an "undated inbox" — only sessions that
    // endSession couldn't date (because the clock was invalid at the time) show
    // up here, most-recent first. Picking a date in the editor moves the session
    // out of this list and into the per-book reading-days bucket for that day.
    const auto& fullSessions = READING_STATS.getSessionLog();
    const auto undated = collectUndatedSessionIndices();
    const int totalUndated = static_cast<int>(undated.size());
    const int sessionCount = std::min(totalUndated, SESSIONS_PER_PAGE);

    const int inboxTop = bucketCardsTop + BUCKET_CARD_HEIGHT + SECTION_GAP;
    const std::string sessionsLabel = std::string(tr(STR_DATE_NOT_SET)) + " (" + std::to_string(totalUndated) + ")";
    GUI.drawSubHeader(renderer, Rect{0, inboxTop, pageWidth, LIST_HEADER_HEIGHT}, sessionsLabel.c_str(), nullptr);

    const Rect contentRect = listRect();
    if (sessionCount == 0) {
      renderer.drawText(UI_10_FONT_ID, sidePadding, contentRect.y + 20, tr(STR_NO_READING_STATS));
    } else {
      // Helper closures resolve session metadata via the undated-indices view.
      auto sessionAt = [&fullSessions, &undated](const int displayIndex) -> const ReadingSessionLogEntry& {
        return fullSessions[undated[static_cast<size_t>(displayIndex)]];
      };
      auto resolveBookTitle = [](const std::string& bookId) -> std::string {
        if (bookId.empty()) {
          return std::string(tr(STR_UNKNOWN));
        }
        for (const auto& b : READING_STATS.getBooks()) {
          if (b.bookId == bookId) {
            return b.title.empty() ? b.path : b.title;
          }
        }
        return std::string(tr(STR_UNKNOWN));
      };

      // -1 highlights no row when the ribbon is focused.
      const int highlightedRow = (selectedItemIndex > 0) ? (selectedItemIndex - 1) : -1;
      GUI.drawList(
          renderer, contentRect, sessionCount, highlightedRow,
          [&](const int index) { return resolveBookTitle(sessionAt(index).bookId); },
          [&](const int /*index*/) { return std::string(tr(STR_DATE_NOT_SET)); }, nullptr,
          [&](const int index) {
            return ReadingStatsAnalytics::formatDurationHm(static_cast<uint64_t>(sessionAt(index).sessionMs));
          },
          false);
    }
  } else if (currentPage == PAGE_YEAR) {
    // Dedicated annual overview: year total + best month, then the year's
    // monthly reading as a bar chart. (Yearly stats live here, not on Monthly.)
    int annualYear = 0;
    const auto annualBars = getAnnualReadingBars(annualYear);
    uint64_t yearTotalMs = 0;
    uint64_t bestMonthMs = 0;
    std::string bestMonthLabel = "-";
    for (const auto& bar : annualBars) {
      yearTotalMs += bar.readingMs;
      if (bar.readingMs > bestMonthMs) {
        bestMonthMs = bar.readingMs;
        bestMonthLabel = bar.bottomLabel;
      }
    }

    drawMetricRow(renderer, Rect{sidePadding, contentTop, contentWidth, SUMMARY_ROW_HEIGHT}, Receipttotal24Icon,
                  tr(STR_YEAR_TOTAL), ReadingStatsAnalytics::formatDurationHm(yearTotalMs));
    const std::string bestMonthValue =
        (bestMonthMs == 0) ? std::string("-")
                           : (bestMonthLabel + " (" + ReadingStatsAnalytics::formatDurationHm(bestMonthMs) + ")");
    drawMetricRow(renderer, Rect{sidePadding, contentTop + SUMMARY_ROW_HEIGHT, contentWidth, SUMMARY_ROW_HEIGHT},
                  Award24Icon, tr(STR_BEST_MONTH), bestMonthValue);

    const int chartHeaderTop = contentTop + SUMMARY_ROW_HEIGHT * 2 + SUMMARY_GAP * 2;
    const int chartTop = chartHeaderTop + CHART_HEADER_HEIGHT + CHART_TOP_GAP;
    const int chartHeight = std::max(120, contentBottom - chartTop);
    GUI.drawSubHeader(renderer, Rect{0, chartHeaderTop, pageWidth, CHART_HEADER_HEIGHT},
                      formatAnnualReadingTitle(annualYear).c_str(), nullptr);
    drawReadingChart(renderer, Rect{sidePadding, chartTop, contentWidth, chartHeight}, annualBars, false);
  }

  // Confirm advances the tab whenever the ribbon (selectedItemIndex == 0) is
  // focused, so its hint is the next tab's name — same convention as
  // SettingsActivity. On the Books/Sessions content rows Confirm selects the
  // row instead, so it reads "Select" there.
  const char* nextTabName = I18N.get(TAB_NAMES[(currentPage + 1) % TOTAL_STATS_PAGES]);

  std::string btn2;
  std::string btn3;
  std::string btn4;
  if (currentPage == PAGE_STARTED_BOOKS || currentPage == PAGE_SESSIONS) {
    btn2 = (selectedItemIndex == 0) ? nextTabName : tr(STR_SELECT);
    btn3 = tr(STR_DIR_UP);
    btn4 = tr(STR_DIR_DOWN);
  } else if (currentPage == PAGE_MONTHLY) {
    // Up/Down step the viewed month; Left/Right do nothing here, so leave them blank.
    btn2 = nextTabName;
    btn3 = tr(STR_DIR_UP);
    btn4 = tr(STR_DIR_DOWN);
  } else if (maxScroll > 0) {
    // Overview/Weekly scroll when their content overflows; Up/Down (or
    // Left/Right) move the view.
    btn2 = nextTabName;
    btn3 = tr(STR_DIR_UP);
    btn4 = tr(STR_DIR_DOWN);
  } else {
    // Non-scrolling tabs: only the next-tab Confirm hint is meaningful.
    btn2 = nextTabName;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), btn2.c_str(), btn3.c_str(), btn4.c_str());
  drawLyraStyleButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // First paint after a forced portrait flip needs a full refresh to clear the
  // previous orientation's image.
  const HalDisplay::RefreshMode refreshMode = fullRefreshNext ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH;
  fullRefreshNext = false;

  // Match SettingsActivity — partial (FAST) refresh on every tab transition.
  // The earlier HALF_REFRESH on non-Books pages was a leftover from when each
  // tab change effectively redrew the whole screen via the bottom N/N
  // pagination; with the top tab bar in place the diff between pages is
  // small enough that FAST keeps the e-ink updates snappy without ghosting.
  renderer.displayBuffer(refreshMode);
}