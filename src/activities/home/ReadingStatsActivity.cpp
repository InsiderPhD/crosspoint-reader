#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Format seconds into a human-readable string: "0m", "45m", "2h 3m", "1d 4h"
void formatDuration(uint32_t totalSeconds, char* buf, size_t bufSize) {
  const uint32_t mins = (totalSeconds + 30) / 60;
  if (mins < 60) {
    snprintf(buf, bufSize, "%um", static_cast<unsigned>(mins < 1 ? 0 : mins));
  } else if (mins < 60 * 24) {
    snprintf(buf, bufSize, "%uh %um", static_cast<unsigned>(mins / 60), static_cast<unsigned>(mins % 60));
  } else {
    const uint32_t hours = mins / 60;
    snprintf(buf, bufSize, "%ud %uh", static_cast<unsigned>(hours / 24), static_cast<unsigned>(hours % 24));
  }
}

}  // namespace

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void ReadingStatsActivity::onExit() { Activity::onExit(); }

void ReadingStatsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    activityManager.goHome();
  }
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_STATS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // Format stat values
  char timeBuf[16];
  formatDuration(READING_STATS.totalReadingTimeSeconds, timeBuf, sizeof(timeBuf));

  char pagesBuf[12];
  snprintf(pagesBuf, sizeof(pagesBuf), "%lu", static_cast<unsigned long>(READING_STATS.totalPagesRead));

  char finishedBuf[12];
  snprintf(finishedBuf, sizeof(finishedBuf), "%lu", static_cast<unsigned long>(READING_STATS.booksFinished));

  char sessionsBuf[12];
  snprintf(sessionsBuf, sizeof(sessionsBuf), "%lu", static_cast<unsigned long>(READING_STATS.totalSessions));

  char avgSessionBuf[16];
  if (READING_STATS.totalSessions > 0) {
    formatDuration(READING_STATS.totalReadingTimeSeconds / READING_STATS.totalSessions, avgSessionBuf,
                   sizeof(avgSessionBuf));
  } else {
    snprintf(avgSessionBuf, sizeof(avgSessionBuf), "--");
  }

  char speedBuf[16];
  if (SETTINGS.readingSpeedSecondsPerPage == 0) {
    snprintf(speedBuf, sizeof(speedBuf), "--");
  } else if (SETTINGS.readingSpeedSecondsPerPage < 60) {
    snprintf(speedBuf, sizeof(speedBuf), "~%us/pg", static_cast<unsigned>(SETTINGS.readingSpeedSecondsPerPage));
  } else {
    snprintf(speedBuf, sizeof(speedBuf), "~%um/pg", static_cast<unsigned>(SETTINGS.readingSpeedSecondsPerPage / 60));
  }

  int inProgressCount = 0;
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (book.progressPercent > 0 && book.progressPercent < 90) inProgressCount++;
  }
  char inProgressBuf[8];
  snprintf(inProgressBuf, sizeof(inProgressBuf), "%d", inProgressCount);

  const char* labels[] = {tr(STR_STATS_READING_TIME),    tr(STR_STATS_PAGES_READ),    tr(STR_STATS_BOOKS_FINISHED),
                          tr(STR_STATS_SESSIONS),         tr(STR_STATS_AVG_SESSION),   tr(STR_STATS_READING_SPEED),
                          tr(STR_STATS_BOOKS_IN_PROGRESS)};
  const char* values[] = {timeBuf, pagesBuf, finishedBuf, sessionsBuf, avgSessionBuf, speedBuf, inProgressBuf};
  constexpr int ROW_COUNT = 7;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, ROW_COUNT, -1,
      [&labels](int i) { return std::string(labels[i]); }, nullptr, nullptr,
      [&values](int i) { return std::string(values[i]); });

  const auto hints = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
