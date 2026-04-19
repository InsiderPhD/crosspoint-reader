#include "ResetStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "activities/home/ReadingStatsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ResetStatsActivity::onEnter() {
  Activity::onEnter();
  state = WARNING;
  requestUpdate();
}

void ResetStatsActivity::onExit() { Activity::onExit(); }

void ResetStatsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_RESET_READING_STATS));

  if (state == WARNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_RESET_STATS_WARNING_1), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_RESET_STATS_WARNING_2), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  if (state == RESETTING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_RESETTING_STATS));
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_STATS_RESET), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_RESET_STATS_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CHECK_SERIAL_OUTPUT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }
}

void ResetStatsActivity::resetStats() {
  LOG_DBG("RESET_STATS", "Resetting reading stats to zero");

  READING_STATS.totalReadingTimeSeconds = 0;
  READING_STATS.totalPagesRead = 0;
  READING_STATS.booksFinished = 0;
  READING_STATS.totalSessions = 0;

  if (!READING_STATS.saveToFile()) {
    LOG_ERR("RESET_STATS", "Failed to save reset stats to file");
    state = FAILED;
    requestUpdate();
    return;
  }

  // Invalidate the in-memory stats cache so ReadingStatsActivity reflects the reset
  ReadingStatsActivity::invalidateCache();

  LOG_DBG("RESET_STATS", "Stats reset successfully");
  state = SUCCESS;
  requestUpdate();
}

void ResetStatsActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("RESET_STATS", "User confirmed stats reset");
      {
        RenderLock lock(*this);
        state = RESETTING;
      }
      requestUpdateAndWait();
      resetStats();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }
}
