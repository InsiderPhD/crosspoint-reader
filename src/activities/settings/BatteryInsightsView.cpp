#include "BatteryInsightsView.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "fontIds.h"
#include "util/PowerHistory.h"

namespace {
constexpr int CARD_GAP = 10;
constexpr int CARD_PADDING = 10;
constexpr int CARD_TITLE_HEIGHT = 34;
constexpr int SUMMARY_HEIGHT = 78;
constexpr int PAGES_HEIGHT = CARD_TITLE_HEIGHT + 40;
constexpr int MIN_GRAPH_HEIGHT = 150;

void drawCentered(const GfxRenderer& renderer, const int fontId, const int x, const int width, const int y,
                  const char* text, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  renderer.drawText(fontId, x + (width - renderer.getTextWidth(fontId, text, style)) / 2, y, text, true, style);
}

// Card frame with a bold title on the left and an optional note on the right.
void drawCard(const GfxRenderer& renderer, const int x, const int y, const int width, const int height,
              const char* title, const char* note) {
  renderer.drawRect(x, y, width, height);
  renderer.drawLine(x, y + CARD_TITLE_HEIGHT, x + width - 1, y + CARD_TITLE_HEIGHT);
  const int titleY = y + (CARD_TITLE_HEIGHT - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  renderer.drawText(UI_10_FONT_ID, x + CARD_PADDING, titleY, title, true, EpdFontFamily::BOLD);
  if (note) {
    const int titleWidth = renderer.getTextWidth(UI_10_FONT_ID, title, EpdFontFamily::BOLD);
    const int noteWidth = renderer.getTextWidth(SMALL_FONT_ID, note);
    if (titleWidth + noteWidth + CARD_PADDING * 3 <= width) {
      renderer.drawText(SMALL_FONT_ID, x + width - CARD_PADDING - noteWidth,
                        y + (CARD_TITLE_HEIGHT - renderer.getLineHeight(SMALL_FONT_ID)) / 2, note);
    }
  }
}

void formatHours(const uint32_t minutes, char* buffer, const size_t size) {
  snprintf(buffer, size, "%luh", static_cast<unsigned long>((minutes + 30U) / 60U));
}

void drawSummary(const GfxRenderer& renderer, const uint8_t levelNow, const uint32_t activeSeconds,
                 const char* periodLabel, const int x, const int y, const int width) {
  renderer.drawRect(x, y, width, SUMMARY_HEIGHT);
  const int columnWidth = width / 3;
  renderer.drawLine(x + columnWidth, y + 10, x + columnWidth, y + SUMMARY_HEIGHT - 10);
  renderer.drawLine(x + columnWidth * 2, y + 10, x + columnWidth * 2, y + SUMMARY_HEIGHT - 10);

  char battery[8];
  snprintf(battery, sizeof(battery), "%u%%", static_cast<unsigned>(levelNow));

  char active[24];
  snprintf(active, sizeof(active), "%luh %02lum", static_cast<unsigned long>(activeSeconds / 3600U),
           static_cast<unsigned long>((activeSeconds % 3600U) / 60U));

  char days[24];
  uint32_t elapsedDays = 0;
  if (PowerHistory::elapsedDaysForDisplay(elapsedDays)) {
    snprintf(days, sizeof(days), "%lu %s", static_cast<unsigned long>(elapsedDays), tr(STR_STAT_DAYS));
  } else {
    snprintf(days, sizeof(days), "--");
  }

  const int valueY = y + 12;
  const int labelY = y + SUMMARY_HEIGHT - renderer.getLineHeight(SMALL_FONT_ID) - 10;
  const int lastColumnWidth = width - columnWidth * 2;
  drawCentered(renderer, UI_12_FONT_ID, x, columnWidth, valueY, battery, EpdFontFamily::BOLD);
  drawCentered(renderer, UI_12_FONT_ID, x + columnWidth, columnWidth, valueY, active, EpdFontFamily::BOLD);
  drawCentered(renderer, UI_12_FONT_ID, x + columnWidth * 2, lastColumnWidth, valueY, days, EpdFontFamily::BOLD);
  drawCentered(renderer, SMALL_FONT_ID, x, columnWidth, labelY, tr(STR_POWER_LEVEL_NOW));
  drawCentered(renderer, SMALL_FONT_ID, x + columnWidth, columnWidth, labelY, tr(STR_POWER_ACTIVE_TIME));
  drawCentered(renderer, SMALL_FONT_ID, x + columnWidth * 2, lastColumnWidth, labelY, periodLabel);
}

void drawGraph(const GfxRenderer& renderer, const PowerHistoryState& history, const uint8_t levelNow,
               const uint32_t activeSeconds, const int x, const int y, const int width, const int height) {
  drawCard(renderer, x, y, width, height, tr(STR_POWER_ACTIVITY_GRAPH), nullptr);

  if (history.sampleCount == 0 || history.lastPercent > 100) {
    drawCentered(renderer, UI_10_FONT_ID, x, width, y + CARD_TITLE_HEIGHT + (height - CARD_TITLE_HEIGHT) / 2 - 10,
                 tr(STR_POWER_WAITING_FOR_SAMPLE));
    return;
  }

  const int axisLabelHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int plotLeft = x + 42;
  const int plotRight = x + width - 14;
  const int plotTop = y + CARD_TITLE_HEIGHT + 14;
  const int plotBottom = y + height - axisLabelHeight - 12;
  const int plotWidth = std::max(1, plotRight - plotLeft);
  const int plotHeight = std::max(1, plotBottom - plotTop);

  static constexpr uint8_t LEVELS[] = {100, 75, 50, 25, 0};
  for (const uint8_t level : LEVELS) {
    const int levelY = plotBottom - plotHeight * level / 100;
    char label[8];
    snprintf(label, sizeof(label), "%u", static_cast<unsigned>(level));
    renderer.drawText(SMALL_FONT_ID, x + CARD_PADDING, levelY - axisLabelHeight / 2, label);
    for (int dashX = plotLeft; dashX < plotRight; dashX += 8) {
      renderer.drawLine(dashX, levelY, std::min(dashX + 3, plotRight), levelY);
    }
  }

  const uint32_t currentMinutes = activeSeconds / 60U;
  const uint32_t lastStoredMinutes = history.sampleActiveMinutes[history.sampleCount - 1];
  const uint32_t maxMinutes = std::max<uint32_t>(1, std::max(currentMinutes, lastStoredMinutes));

  char axisLabel[12];
  formatHours(0, axisLabel, sizeof(axisLabel));
  renderer.drawText(SMALL_FONT_ID, plotLeft, plotBottom + 5, axisLabel);
  formatHours(maxMinutes / 2U, axisLabel, sizeof(axisLabel));
  drawCentered(renderer, SMALL_FONT_ID, plotLeft, plotWidth, plotBottom + 5, axisLabel);
  formatHours(maxMinutes, axisLabel, sizeof(axisLabel));
  renderer.drawText(SMALL_FONT_ID, plotRight - renderer.getTextWidth(SMALL_FONT_ID, axisLabel), plotBottom + 5,
                    axisLabel);

  auto pointX = [plotLeft, plotWidth, maxMinutes](const uint32_t minutes) {
    return plotLeft + static_cast<int>(static_cast<uint64_t>(plotWidth) * minutes / maxMinutes);
  };
  auto pointY = [plotBottom, plotHeight](const uint8_t percent) { return plotBottom - plotHeight * percent / 100; };

  int previousX = pointX(history.sampleActiveMinutes[0]);
  int previousY = pointY(history.samplePercents[0]);
  renderer.fillRect(previousX - 2, previousY - 2, 5, 5);
  for (uint8_t i = 1; i < history.sampleCount; ++i) {
    const int sampleX = pointX(history.sampleActiveMinutes[i]);
    const int sampleY = pointY(history.samplePercents[i]);
    renderer.drawLine(previousX, previousY, sampleX, sampleY, 2, true);
    renderer.fillRect(sampleX - 2, sampleY - 2, 5, 5);
    previousX = sampleX;
    previousY = sampleY;
  }

  // Run the line on to now: the current level at the current awake time.
  if (levelNow != history.samplePercents[history.sampleCount - 1] || currentMinutes != lastStoredMinutes) {
    const int currentX = pointX(currentMinutes);
    const int currentY = pointY(levelNow);
    renderer.drawLine(previousX, previousY, currentX, currentY, 2, true);
    renderer.fillRect(currentX - 2, currentY - 2, 5, 5);
  }
}
}  // namespace

void BatteryInsightsView::draw(GfxRenderer& renderer, const int x, const int y, const int width, const int height,
                               const uint8_t batteryPercent) {
  const PowerHistoryState& history = PowerHistory::historyForDisplay();
  // The simulator's demo cycle carries its own level.
  const uint8_t levelNow = PowerHistory::isDemoMode() ? history.lastPercent : batteryPercent;
  const uint32_t activeSeconds = PowerHistory::activeSecondsForDisplay();
  // Until a charge has been seen, the figures cover "since tracking began".
  const char* periodLabel = history.cycleConfirmed ? tr(STR_POWER_SINCE_CHARGE) : tr(STR_POWER_SINCE_TRACKING);

  drawSummary(renderer, levelNow, activeSeconds, periodLabel, x, y, width);

  const int graphY = y + SUMMARY_HEIGHT + CARD_GAP;
  const int graphHeight = std::max(MIN_GRAPH_HEIGHT, height - SUMMARY_HEIGHT - PAGES_HEIGHT - CARD_GAP * 2);
  drawGraph(renderer, history, levelNow, activeSeconds, x, graphY, width, graphHeight);

  const int pagesY = graphY + graphHeight + CARD_GAP;
  drawCard(renderer, x, pagesY, width, PAGES_HEIGHT, tr(STR_POWER_PAGES_TURNED), periodLabel);
  char count[16];
  snprintf(count, sizeof(count), "%lu", static_cast<unsigned long>(PowerHistory::pageTurnsForDisplay()));
  drawCentered(
      renderer, UI_12_FONT_ID, x, width,
      pagesY + CARD_TITLE_HEIGHT + (PAGES_HEIGHT - CARD_TITLE_HEIGHT - renderer.getLineHeight(UI_12_FONT_ID)) / 2,
      count, EpdFontFamily::BOLD);
}
