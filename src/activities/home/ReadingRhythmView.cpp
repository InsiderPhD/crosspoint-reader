#include "ReadingRhythmView.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "ReadingStatsStore.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

namespace {
constexpr int WEEK_COUNT = 13;
constexpr int DAY_COUNT = WEEK_COUNT * 7;
constexpr int MONTH_COUNT = 12;
// Dot calendar steps: any reading, a quarter hour, half an hour or more.
constexpr unsigned MEDIUM_MINUTES = 15;
constexpr unsigned HIGH_MINUTES = 30;

constexpr int CARD_GAP = 10;
constexpr int CARD_PADDING = 10;
constexpr int CARD_TITLE_HEIGHT = 34;
constexpr int WEEKDAY_LABEL_WIDTH = 44;
constexpr int MONTH_LABEL_HEIGHT = 24;
constexpr int ROW_STRIDE = 24;
constexpr int LEGEND_HEIGHT = 34;
constexpr int ACTIVE_DOT = 14;
constexpr int IDLE_DOT = 5;
constexpr int TODAY_RING = ACTIVE_DOT + 6;
constexpr int WEEK_BAR_WIDTH = 12;
constexpr int MONTH_BAR_WIDTH = 11;
constexpr int ACTIVITY_CARD_HEIGHT =
    CARD_TITLE_HEIGHT + MONTH_LABEL_HEIGHT + ROW_STRIDE * 7 + LEGEND_HEIGHT + CARD_PADDING;
constexpr int WEEKLY_CARD_HEIGHT = CARD_TITLE_HEIGHT + 86;
constexpr int MONTHLY_CARD_HEIGHT = CARD_TITLE_HEIGHT + 96;
// Spare viewport height goes to the two bar charts, up to this much in total.
constexpr int MAX_CHART_STRETCH = 120;

constexpr StrId WEEKDAY_LABELS[7] = {StrId::STR_WEEKDAY_SHORT_MON, StrId::STR_WEEKDAY_SHORT_TUE,
                                     StrId::STR_WEEKDAY_SHORT_WED, StrId::STR_WEEKDAY_SHORT_THU,
                                     StrId::STR_WEEKDAY_SHORT_FRI, StrId::STR_WEEKDAY_SHORT_SAT,
                                     StrId::STR_WEEKDAY_SHORT_SUN};
constexpr StrId MONTH_LABELS[12] = {StrId::STR_MONTH_SHORT_JAN, StrId::STR_MONTH_SHORT_FEB, StrId::STR_MONTH_SHORT_MAR,
                                    StrId::STR_MONTH_SHORT_APR, StrId::STR_MONTH_SHORT_MAY, StrId::STR_MONTH_SHORT_JUN,
                                    StrId::STR_MONTH_SHORT_JUL, StrId::STR_MONTH_SHORT_AUG, StrId::STR_MONTH_SHORT_SEP,
                                    StrId::STR_MONTH_SHORT_OCT, StrId::STR_MONTH_SHORT_NOV, StrId::STR_MONTH_SHORT_DEC};

// Everything the three cards show, gathered in one pass over the day buckets.
// ~200 bytes, lives on the render task's stack for the duration of one draw.
struct RhythmData {
  uint32_t firstDay = 0;  // Monday of the oldest calendar column
  uint16_t dayMinutes[DAY_COUNT] = {};
  uint8_t monthDays[MONTH_COUNT] = {};    // days with any reading, oldest month first
  uint8_t monthNumber[MONTH_COUNT] = {};  // 1-12, for the axis labels
};

unsigned toMinutes(const uint64_t readingMs) {
  if (readingMs == 0) {
    return 0;
  }
  // A day with under a minute logged still counts as a reading day.
  return static_cast<unsigned>(std::clamp<uint64_t>(readingMs / 60000ULL, 1, UINT16_MAX));
}

RhythmData collect(const uint32_t referenceDay) {
  RhythmData data;
  const uint32_t weekday = (referenceDay + 3U) % 7U;  // Monday = 0
  data.firstDay = referenceDay - weekday - static_cast<uint32_t>(WEEK_COUNT - 1) * 7U;

  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  TimeUtils::getDateFromDayOrdinal(referenceDay, year, month, day);
  uint32_t monthStart[MONTH_COUNT + 1];
  const int firstSerial = year * 12 + static_cast<int>(month) - 1 - (MONTH_COUNT - 1);
  for (int i = 0; i <= MONTH_COUNT; ++i) {
    const int serial = firstSerial + i;
    monthStart[i] = TimeUtils::getDayOrdinalForDate(serial / 12, static_cast<unsigned>(serial % 12) + 1, 1);
    if (i < MONTH_COUNT) {
      data.monthNumber[i] = static_cast<uint8_t>(serial % 12 + 1);
    }
  }

  for (const auto& readingDay : READING_STATS.getReadingDays()) {
    const uint32_t ordinal = readingDay.dayOrdinal;
    if (readingDay.readingMs == 0 || ordinal > referenceDay) {
      continue;
    }
    if (ordinal >= data.firstDay) {
      data.dayMinutes[ordinal - data.firstDay] = static_cast<uint16_t>(toMinutes(readingDay.readingMs));
    }
    for (int i = 0; i < MONTH_COUNT; ++i) {
      if (ordinal >= monthStart[i] && ordinal < monthStart[i + 1]) {
        data.monthDays[i]++;
        break;
      }
    }
  }
  return data;
}

void formatMinutes(const unsigned minutes, char* buffer, const size_t size) {
  if (minutes < 60) {
    snprintf(buffer, size, "%um", minutes);
  } else if (minutes % 60 == 0) {
    snprintf(buffer, size, "%uh", minutes / 60);
  } else {
    snprintf(buffer, size, "%uh %um", minutes / 60, minutes % 60);
  }
}

unsigned weeklyScaleMinutes(const unsigned maxMinutes) {
  if (maxMinutes <= 60) return 60;
  if (maxMinutes <= 180) return 180;
  if (maxMinutes <= 360) return 360;
  if (maxMinutes <= 600) return 600;
  return (maxMinutes + 119U) / 120U * 120U;
}

void drawCentered(const GfxRenderer& renderer, const int x, const int width, const int y, const char* text) {
  renderer.drawText(SMALL_FONT_ID, x + (width - renderer.getTextWidth(SMALL_FONT_ID, text)) / 2, y, text);
}

void drawCard(const GfxRenderer& renderer, const int x, const int y, const int width, const int height,
              const char* title) {
  renderer.drawRect(x, y, width, height);
  renderer.drawLine(x, y + CARD_TITLE_HEIGHT, x + width - 1, y + CARD_TITLE_HEIGHT);
  const int titleWidth = renderer.getTextWidth(UI_10_FONT_ID, title, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, x + (width - titleWidth) / 2,
                    y + (CARD_TITLE_HEIGHT - renderer.getLineHeight(UI_10_FONT_ID)) / 2, title, true,
                    EpdFontFamily::BOLD);
}

// "label value" pairs along a card's bottom edge, one flush left, one flush right.
void drawFooter(const GfxRenderer& renderer, const int x, const int width, const int y, const char* leftLabel,
                const char* leftValue, const char* rightLabel, const char* rightValue) {
  constexpr int GAP = 4;
  renderer.drawText(SMALL_FONT_ID, x, y, leftLabel);
  renderer.drawText(SMALL_FONT_ID, x + renderer.getTextWidth(SMALL_FONT_ID, leftLabel) + GAP, y, leftValue);
  const int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, rightLabel);
  const int rightX = x + width - rightLabelWidth - GAP - renderer.getTextWidth(SMALL_FONT_ID, rightValue);
  renderer.drawText(SMALL_FONT_ID, rightX, y, rightLabel);
  renderer.drawText(SMALL_FONT_ID, rightX + rightLabelWidth + GAP, y, rightValue);
}

void drawDot(const GfxRenderer& renderer, const int centerX, const int centerY, const unsigned minutes) {
  int size = IDLE_DOT;
  Color color = Color::LightGray;
  if (minutes >= HIGH_MINUTES) {
    size = ACTIVE_DOT;
    color = Color::Black;
  } else if (minutes >= MEDIUM_MINUTES) {
    size = ACTIVE_DOT - 2;
    color = Color::DarkGray;
  } else if (minutes > 0) {
    size = ACTIVE_DOT - 4;
  }
  renderer.fillRoundedRect(centerX - size / 2, centerY - size / 2, size, size, size / 2, color);
}

void drawTodayRing(const GfxRenderer& renderer, const int centerX, const int centerY) {
  renderer.drawRoundedRect(centerX - TODAY_RING / 2, centerY - TODAY_RING / 2, TODAY_RING, TODAY_RING, 2,
                           TODAY_RING / 2, true);
}

void drawActivityCard(const GfxRenderer& renderer, const int x, const int y, const int width, const RhythmData& data,
                      const uint32_t referenceDay, const bool markToday, const int gridX, const int columnStride) {
  drawCard(renderer, x, y, width, ACTIVITY_CARD_HEIGHT, tr(STR_STATS_RECENT_ACTIVITY));

  // Month name above the first column of each month. The oldest column only
  // gets one when its month runs on, so two names never collide at the edge.
  unsigned previousMonth = 0;
  for (int week = 0; week < WEEK_COUNT; ++week) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    TimeUtils::getDateFromDayOrdinal(data.firstDay + static_cast<uint32_t>(week) * 7U, year, month, day);
    if (month == previousMonth || month < 1 || month > 12) {
      continue;
    }
    previousMonth = month;
    if (week == 0 && day > 21) {
      continue;
    }
    renderer.drawText(SMALL_FONT_ID, gridX + week * columnStride, y + CARD_TITLE_HEIGHT + 3,
                      I18N.get(MONTH_LABELS[month - 1]));
  }

  const int gridTop = y + CARD_TITLE_HEIGHT + MONTH_LABEL_HEIGHT;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  for (int weekday = 0; weekday < 7; ++weekday) {
    const int centerY = gridTop + weekday * ROW_STRIDE + ROW_STRIDE / 2;
    renderer.drawText(SMALL_FONT_ID, x + CARD_PADDING, centerY - lineHeight / 2, I18N.get(WEEKDAY_LABELS[weekday]));
    for (int week = 0; week < WEEK_COUNT; ++week) {
      const int index = week * 7 + weekday;
      const uint32_t ordinal = data.firstDay + static_cast<uint32_t>(index);
      if (ordinal > referenceDay) {
        continue;
      }
      const int centerX = gridX + week * columnStride + columnStride / 2;
      drawDot(renderer, centerX, centerY, data.dayMinutes[index]);
      if (markToday && ordinal == referenceDay) {
        drawTodayRing(renderer, centerX, centerY);
      }
    }
  }

  // Legend: one sample dot per step on the left, the today ring on the right.
  const int legendCenterY = gridTop + ROW_STRIDE * 7 + LEGEND_HEIGHT / 2;
  const int legendTextY = legendCenterY - lineHeight / 2;
  int legendX = x + CARD_PADDING;
  renderer.drawText(SMALL_FONT_ID, legendX, legendTextY, tr(STR_STATS_MINUTES));
  legendX += renderer.getTextWidth(SMALL_FONT_ID, tr(STR_STATS_MINUTES)) + 8;
  static constexpr unsigned LEGEND_STEPS[] = {1, MEDIUM_MINUTES, HIGH_MINUTES};
  for (const unsigned step : LEGEND_STEPS) {
    char label[8];
    snprintf(label, sizeof(label), step == HIGH_MINUTES ? "%u+" : "%u", step);
    drawDot(renderer, legendX + ACTIVE_DOT / 2, legendCenterY, step);
    legendX += ACTIVE_DOT + 4;
    renderer.drawText(SMALL_FONT_ID, legendX, legendTextY, label);
    legendX += renderer.getTextWidth(SMALL_FONT_ID, label) + 10;
  }
  if (markToday) {
    const int todayX = x + width - CARD_PADDING - renderer.getTextWidth(SMALL_FONT_ID, tr(STR_TODAY));
    renderer.drawText(SMALL_FONT_ID, todayX, legendTextY, tr(STR_TODAY));
    drawTodayRing(renderer, todayX - 6 - TODAY_RING / 2, legendCenterY);
  }
}

void drawWeeklyCard(const GfxRenderer& renderer, const int x, const int y, const int width, const int height,
                    const RhythmData& data, const int gridX, const int columnStride) {
  drawCard(renderer, x, y, width, height, tr(STR_STATS_WEEKLY_TIME));

  unsigned weekMinutes[WEEK_COUNT] = {};
  unsigned maxMinutes = 0;
  unsigned earlierMinutes = 0;
  for (int week = 0; week < WEEK_COUNT; ++week) {
    for (int weekday = 0; weekday < 7; ++weekday) {
      weekMinutes[week] += data.dayMinutes[week * 7 + weekday];
    }
    maxMinutes = std::max(maxMinutes, weekMinutes[week]);
    if (week < WEEK_COUNT - 1) {
      earlierMinutes += weekMinutes[week];
    }
  }
  // The average excludes the week in progress, which would drag it down.
  const unsigned averageMinutes = earlierMinutes / (WEEK_COUNT - 1);
  const unsigned scaleMinutes = weeklyScaleMinutes(maxMinutes);

  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int chartTop = y + CARD_TITLE_HEIGHT + 8;
  const int footerY = y + height - lineHeight - 4;
  const int chartBottom = footerY - 5;
  const int chartHeight = std::max(1, chartBottom - chartTop);
  const int chartRight = gridX + columnStride * WEEK_COUNT;

  char scaleLabel[16];
  formatMinutes(scaleMinutes, scaleLabel, sizeof(scaleLabel));
  renderer.drawText(SMALL_FONT_ID, x + CARD_PADDING, chartTop, scaleLabel);
  renderer.drawLine(gridX, chartBottom, chartRight, chartBottom);

  for (int week = 0; week < WEEK_COUNT; ++week) {
    if (weekMinutes[week] == 0) {
      continue;
    }
    const int barHeight = std::clamp(static_cast<int>(chartHeight * weekMinutes[week] / scaleMinutes), 2, chartHeight);
    const int barX = gridX + week * columnStride + (columnStride - WEEK_BAR_WIDTH) / 2;
    if (week == WEEK_COUNT - 1) {
      renderer.fillRect(barX, chartBottom - barHeight, WEEK_BAR_WIDTH, barHeight);
    } else {
      renderer.fillRectDither(barX, chartBottom - barHeight, WEEK_BAR_WIDTH, barHeight, Color::DarkGray);
    }
  }

  // Dashed average line, drawn over the bars so it stays readable across them.
  if (averageMinutes > 0) {
    const int averageY =
        std::min(chartBottom - 3, chartBottom - static_cast<int>(chartHeight * averageMinutes / scaleMinutes));
    for (int dashX = gridX; dashX < chartRight; dashX += 8) {
      renderer.fillRect(dashX - 1, averageY - 1, 6, 3, false);
      renderer.drawLine(dashX, averageY, std::min(dashX + 3, chartRight), averageY);
    }
  }

  char averageValue[16];
  char currentValue[16];
  formatMinutes(averageMinutes, averageValue, sizeof(averageValue));
  formatMinutes(weekMinutes[WEEK_COUNT - 1], currentValue, sizeof(currentValue));
  drawFooter(renderer, x + CARD_PADDING, width - CARD_PADDING * 2, footerY, tr(STR_STATS_WEEKLY_AVERAGE), averageValue,
             tr(STR_STATS_THIS_WEEK), currentValue);
}

void drawMonthlyCard(const GfxRenderer& renderer, const int x, const int y, const int width, const int height,
                     const RhythmData& data) {
  drawCard(renderer, x, y, width, height, tr(STR_STATS_READING_DAYS_BY_MONTH));

  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int valueY = y + CARD_TITLE_HEIGHT + 3;
  const int footerY = y + height - lineHeight - 4;
  const int labelY = footerY - lineHeight;
  const int chartTop = valueY + lineHeight + 2;
  const int chartBottom = labelY - 3;
  const int chartHeight = std::max(1, chartBottom - chartTop);
  const int gridX = x + CARD_PADDING;
  const int columnStride = std::max(1, (width - CARD_PADDING * 2) / MONTH_COUNT);

  renderer.drawLine(gridX, chartBottom, gridX + columnStride * MONTH_COUNT, chartBottom);

  unsigned totalDays = 0;
  for (int i = 0; i < MONTH_COUNT; ++i) {
    const unsigned days = data.monthDays[i];
    totalDays += days;
    const int columnX = gridX + i * columnStride;

    char value[4];
    if (days == 0) {
      snprintf(value, sizeof(value), "-");
    } else {
      snprintf(value, sizeof(value), "%u", days);
    }
    drawCentered(renderer, columnX, columnStride, valueY, value);

    if (days > 0) {
      const int barHeight = std::clamp(static_cast<int>(chartHeight * days / 31U), 2, chartHeight);
      renderer.fillRoundedRect(columnX + (columnStride - MONTH_BAR_WIDTH) / 2, chartBottom - barHeight, MONTH_BAR_WIDTH,
                               barHeight, 2, Color::DarkGray);
    }
    drawCentered(renderer, columnX, columnStride, labelY, I18N.get(MONTH_LABELS[data.monthNumber[i] - 1]));
  }

  char totalValue[24];
  char averageValue[24];
  snprintf(totalValue, sizeof(totalValue), "%u %s", totalDays, tr(STR_STAT_DAYS));
  snprintf(averageValue, sizeof(averageValue), "%u %s", (totalDays + MONTH_COUNT / 2) / MONTH_COUNT, tr(STR_STAT_DAYS));
  drawFooter(renderer, x + CARD_PADDING, width - CARD_PADDING * 2, footerY, tr(STR_STATS_MONTHLY_TOTAL), totalValue,
             tr(STR_STATS_MONTHLY_AVERAGE), averageValue);
}
}  // namespace

int ReadingRhythmView::contentHeight() {
  return ACTIVITY_CARD_HEIGHT + CARD_GAP + WEEKLY_CARD_HEIGHT + CARD_GAP + MONTHLY_CARD_HEIGHT;
}

void ReadingRhythmView::draw(GfxRenderer& renderer, const int x, const int y, const int width,
                             const int availableHeight, const uint32_t referenceDayOrdinal, const bool markToday) {
  // Day ordinals count from 1970; anything this small is not a real date and
  // would underflow the 13-week window.
  if (referenceDayOrdinal < 366U) {
    return;
  }
  const RhythmData data = collect(referenceDayOrdinal);

  // The calendar columns and the weekly bars share one x grid, so a week's dot
  // column sits directly above its bar.
  const int gridX = x + CARD_PADDING + WEEKDAY_LABEL_WIDTH;
  const int columnStride = std::max(1, (width - CARD_PADDING * 2 - WEEKDAY_LABEL_WIDTH) / WEEK_COUNT);

  const int stretch = std::clamp(availableHeight - contentHeight(), 0, MAX_CHART_STRETCH);
  const int weeklyHeight = WEEKLY_CARD_HEIGHT + stretch / 2;
  const int monthlyHeight = MONTHLY_CARD_HEIGHT + stretch - stretch / 2;

  drawActivityCard(renderer, x, y, width, data, referenceDayOrdinal, markToday, gridX, columnStride);
  const int weeklyY = y + ACTIVITY_CARD_HEIGHT + CARD_GAP;
  drawWeeklyCard(renderer, x, weeklyY, width, weeklyHeight, data, gridX, columnStride);
  drawMonthlyCard(renderer, x, weeklyY + weeklyHeight + CARD_GAP, width, monthlyHeight, data);
}
