#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <string>
#include <vector>

#include "CrossPointState.h"
#include "ReadingDayDetailActivity.h"
#include "ReadingStatsDetailActivity.h"
#include "ReadingStatsStore.h"
#include "SessionDateEditActivity.h"
#include "activities/util/ConfirmationActivity.h"
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

namespace {
constexpr unsigned long BOOK_LONG_PRESS_MS = 1000;
constexpr int TOTAL_STATS_PAGES = 5;
constexpr int PAGE_OVERVIEW = 0;
constexpr int PAGE_STARTED_BOOKS = 1;
constexpr int PAGE_WEEKLY = 2;
constexpr int PAGE_MONTHLY = 3;
constexpr int PAGE_SESSIONS = 4;

// Tab labels in display order — index matches the PAGE_* enum values above.
constexpr StrId TAB_NAMES[TOTAL_STATS_PAGES] = {
    StrId::STR_STATS_TAB_OVERVIEW, StrId::STR_STATS_TAB_BOOKS, StrId::STR_STATS_TAB_WEEKLY, StrId::STR_MONTH,
    StrId::STR_STATS_TAB_SESSIONS,
};

// Sessions tab is capped at one screenful like the Books tab.
constexpr int SESSIONS_PER_PAGE = 4;

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

void drawLyraStyleButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                              const char* btn4) {
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

  currentPage = PAGE_OVERVIEW;
  selectedItemIndex = 0;
  resolveReferenceMonth(viewedYear, viewedMonth);

  waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  waitForBackRelease = false;
  requestUpdate();
}

void ReadingStatsActivity::onExit() { Activity::onExit(); }

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
    finish();
    return;
  }

  if (waitForConfirmRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      waitForConfirmRelease = false;
    }
    return;
  }

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

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int sidePadding = metrics.contentSidePadding;
  const int contentWidth = pageWidth - sidePadding * 2;
  // Tab bar lives directly below the header. Page content starts after the tab
  // bar; the bottom-of-screen N/N indicator is gone now so we no longer
  // reserve PAGE_INDICATOR_HEIGHT — only buttonHints and a small gap.
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - 4;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_STATS),
                 nullptr);

  std::vector<TabInfo> tabs;
  tabs.reserve(TOTAL_STATS_PAGES);
  for (int i = 0; i < TOTAL_STATS_PAGES; ++i) {
    tabs.push_back({I18N.get(TAB_NAMES[i]), currentPage == i});
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 selectedItemIndex == 0);

  if (currentPage == PAGE_OVERVIEW) {
    const uint64_t todayReadingMs = READING_STATS.getTodayReadingMs();
    int annualReadingYear = 0;
    const auto annualReadingBars = getAnnualReadingBars(annualReadingYear);
    const std::string annualReadingTitle = formatAnnualReadingTitle(annualReadingYear);
    const std::string dailyGoalValue = ReadingStatsAnalytics::formatDurationHm(todayReadingMs) + " / " +
                                       ReadingStatsAnalytics::formatDurationHm(getDailyReadingGoalMs());

    drawMetricRow(renderer, Rect{sidePadding, contentTop, contentWidth, SUMMARY_ROW_HEIGHT}, Streak24Icon,
                  tr(STR_STREAK), std::to_string(READING_STATS.getCurrentStreakDays()));
    drawMetricRow(renderer, Rect{sidePadding, contentTop + SUMMARY_ROW_HEIGHT, contentWidth, SUMMARY_ROW_HEIGHT},
                  Confetti24Icon, tr(STR_MAX_STREAK), std::to_string(READING_STATS.getMaxStreakDays()));
    drawMetricRow(renderer, Rect{sidePadding, contentTop + SUMMARY_ROW_HEIGHT * 2, contentWidth, SUMMARY_ROW_HEIGHT},
                  Checkbox24Icon, tr(STR_DAILY_GOAL), dailyGoalValue);
    drawMetricRow(renderer, Rect{sidePadding, contentTop + SUMMARY_ROW_HEIGHT * 3, contentWidth, SUMMARY_ROW_HEIGHT},
                  Readingtime24Icon, tr(STR_READING_TIME),
                  ReadingStatsAnalytics::formatDurationHm(READING_STATS.getTotalReadingMs()));
    drawMetricRow(renderer, Rect{sidePadding, contentTop + SUMMARY_ROW_HEIGHT * 4, contentWidth, SUMMARY_ROW_HEIGHT},
                  Check24Icon, tr(STR_BOOKS_FINISHED), std::to_string(READING_STATS.getBooksFinishedCount()));
    drawMetricRow(renderer, Rect{sidePadding, contentTop + SUMMARY_ROW_HEIGHT * 5, contentWidth, SUMMARY_ROW_HEIGHT},
                  Files24Icon, tr(STR_BOOKS_STARTED), std::to_string(READING_STATS.getBooksStartedCount()));

    const int chartHeaderTop = contentTop + SUMMARY_ROW_HEIGHT * 6 + SUMMARY_GAP * 2;
    const int chartTop = chartHeaderTop + CHART_HEADER_HEIGHT + CHART_TOP_GAP;
    const int chartHeight = std::max(96, contentBottom - chartTop);
    GUI.drawSubHeader(renderer, Rect{0, chartHeaderTop, pageWidth, CHART_HEADER_HEIGHT}, annualReadingTitle.c_str(),
                      nullptr);
    drawReadingChart(renderer, Rect{sidePadding, chartTop, contentWidth, chartHeight}, annualReadingBars, false);
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

    const int listTop = contentTop + LIST_HEADER_HEIGHT + LIST_HEADER_BOTTOM_GAP;
    if (books.empty()) {
      renderer.drawText(UI_10_FONT_ID, sidePadding, listTop + 20, tr(STR_NO_READING_STATS));
    } else {
      const int maxListHeight = std::max(0, contentBottom - listTop);
      const int targetListHeight = metrics.listWithSubtitleRowHeight * BOOKS_PER_PAGE;
      const int listHeight = std::min(maxListHeight, targetListHeight);
      // -1 highlights no row, which is what we want when the ribbon is focused.
      const int highlightedRow = (selectedItemIndex > 0) ? (selectedItemIndex - 1) : -1;
      GUI.drawList(
          renderer, Rect{0, listTop, pageWidth, listHeight}, bookCount, highlightedRow,
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
    const uint64_t last30DaysValueMs = READING_STATS.getRecentReadingMs(30);

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

    const uint64_t avgDayMs = last7DaysValueMs / 7ULL;
    drawMetricRow(renderer, Rect{sidePadding, contentTop, contentWidth, SUMMARY_ROW_HEIGHT}, Last7days24Icon,
                  tr(STR_LAST_7D), ReadingStatsAnalytics::formatDurationHm(last7DaysValueMs));
    drawMetricRow(renderer, Rect{sidePadding, contentTop + SUMMARY_ROW_HEIGHT, contentWidth, SUMMARY_ROW_HEIGHT},
                  Last30days24Icon, tr(STR_LAST_30D), ReadingStatsAnalytics::formatDurationHm(last30DaysValueMs));
    drawMetricRow(renderer, Rect{sidePadding, contentTop + SUMMARY_ROW_HEIGHT * 2, contentWidth, SUMMARY_ROW_HEIGHT},
                  Book24Icon, tr(STR_DAY_TOTAL), ReadingStatsAnalytics::formatDurationHm(avgDayMs));
    drawMetricRow(renderer, Rect{sidePadding, contentTop + SUMMARY_ROW_HEIGHT * 3, contentWidth, SUMMARY_ROW_HEIGHT},
                  Check24Icon, tr(STR_DAYS_READ), std::to_string(daysRead));
    drawMetricRow(renderer, Rect{sidePadding, contentTop + SUMMARY_ROW_HEIGHT * 4, contentWidth, SUMMARY_ROW_HEIGHT},
                  Checkbox24Icon, tr(STR_DAILY_GOAL), std::to_string(goalDays) + "/7");

    const std::string bestDayValue =
        (bestDayMs == 0) ? std::string("-")
                         : (bestDayLabel + " (" + ReadingStatsAnalytics::formatDurationHm(bestDayMs) + ")");
    drawMetricRow(renderer, Rect{sidePadding, contentTop + SUMMARY_ROW_HEIGHT * 5, contentWidth, SUMMARY_ROW_HEIGHT},
                  Award24Icon, tr(STR_BEST_DAY), bestDayValue);

    const int chartHeaderTop = contentTop + SUMMARY_ROW_HEIGHT * 6 + SUMMARY_GAP * 2;
    const int chartTop = chartHeaderTop + CHART_HEADER_HEIGHT + CHART_TOP_GAP;
    const int chartHeight = std::max(96, contentBottom - chartTop);
    GUI.drawSubHeader(renderer, Rect{0, chartHeaderTop, pageWidth, CHART_HEADER_HEIGHT}, tr(STR_DAILY_READING),
                      nullptr);
    drawReadingChart(renderer, Rect{sidePadding, chartTop, contentWidth, chartHeight}, weekBars, true);
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
    drawMetricRow(renderer, Rect{sidePadding, summaryTop + SUMMARY_ROW_HEIGHT * 3, contentWidth, SUMMARY_ROW_HEIGHT},
                  Last30days24Icon, tr(STR_YEAR),
                  ReadingStatsAnalytics::formatDurationHm(monthSummary.yearTotalReadingMs));

    const int gridTop = summaryTop + SUMMARY_ROW_HEIGHT * 4 + SECTION_GAP;
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
    // The Sessions tab is an "undated inbox" — only sessions that endSession
    // couldn't date (because the clock was invalid at the time) show up here,
    // most-recent first. Picking a date in the editor moves the session out
    // of this list and into the per-book reading-days bucket for that day.
    const auto& fullSessions = READING_STATS.getSessionLog();
    const auto undated = collectUndatedSessionIndices();
    const int totalUndated = static_cast<int>(undated.size());
    const int sessionCount = std::min(totalUndated, SESSIONS_PER_PAGE);

    const std::string sessionsLabel = std::string(tr(STR_DATE_NOT_SET)) + " (" + std::to_string(totalUndated) + ")";
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, LIST_HEADER_HEIGHT}, sessionsLabel.c_str(), nullptr);

    const int listTop = contentTop + LIST_HEADER_HEIGHT + LIST_HEADER_BOTTOM_GAP;
    if (sessionCount == 0) {
      renderer.drawText(UI_10_FONT_ID, sidePadding, listTop + 20, tr(STR_NO_READING_STATS));
    } else {
      const int maxListHeight = std::max(0, contentBottom - listTop);
      const int targetListHeight = metrics.listWithSubtitleRowHeight * SESSIONS_PER_PAGE;
      const int listHeight = std::min(maxListHeight, targetListHeight);

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
          renderer, Rect{0, listTop, pageWidth, listHeight}, sessionCount, highlightedRow,
          [&](const int index) { return resolveBookTitle(sessionAt(index).bookId); },
          [&](const int /*index*/) { return std::string(tr(STR_DATE_NOT_SET)); }, nullptr,
          [&](const int index) {
            return ReadingStatsAnalytics::formatDurationHm(static_cast<uint64_t>(sessionAt(index).sessionMs));
          },
          false);
    }
  }

  // The bottom-of-screen "N/N" indicator is gone — the tab bar at the top is
  // the canonical position indicator now.

  std::string btn2;
  std::string btn3 = tr(STR_DIR_LEFT);
  std::string btn4 = tr(STR_DIR_RIGHT);
  if (currentPage == PAGE_STARTED_BOOKS || currentPage == PAGE_SESSIONS) {
    btn2 = tr(STR_SELECT);
    btn3 = tr(STR_DIR_UP);
    btn4 = tr(STR_DIR_DOWN);
  } else if (currentPage == PAGE_MONTHLY) {
    btn3 = tr(STR_DIR_UP);
    btn4 = tr(STR_DIR_DOWN);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), btn2.c_str(), btn3.c_str(), btn4.c_str());
  drawLyraStyleButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Match SettingsActivity — partial (FAST) refresh on every tab transition.
  // The earlier HALF_REFRESH on non-Books pages was a leftover from when each
  // tab change effectively redrew the whole screen via the bottom N/N
  // pagination; with the top tab bar in place the diff between pages is
  // small enough that FAST keeps the e-ink updates snappy without ghosting.
  renderer.displayBuffer();
}