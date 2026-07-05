#include "StatsDataActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StoryGraphExport.h"

namespace {
// SD-root paths so exports are easy to retrieve over WebDAV / USB.
constexpr const char* JSON_EXPORT_PATH = "/reading_stats_export.json";
constexpr const char* STORYGRAPH_EXPORT_PATH = "/storygraph_export.csv";
}  // namespace

const char* StatsDataActivity::titleText() const {
  switch (mode) {
    case StatsDataMode::ExportJson:
      return tr(STR_EXPORT_READING_STATS);
    case StatsDataMode::ImportJson:
      return tr(STR_IMPORT_READING_STATS);
    case StatsDataMode::ExportStoryGraph:
      return tr(STR_EXPORT_STORYGRAPH);
  }
  return "";
}

const char* StatsDataActivity::confirmText() const {
  switch (mode) {
    case StatsDataMode::ExportJson:
      return tr(STR_EXPORT_READING_STATS_CONFIRM);
    case StatsDataMode::ImportJson:
      return tr(STR_IMPORT_READING_STATS_CONFIRM);
    case StatsDataMode::ExportStoryGraph:
      return tr(STR_EXPORT_STORYGRAPH_CONFIRM);
  }
  return "";
}

const char* StatsDataActivity::workingText() const {
  return mode == StatsDataMode::ImportJson ? tr(STR_IMPORTING) : tr(STR_EXPORTING);
}

const char* StatsDataActivity::successText() const {
  switch (mode) {
    case StatsDataMode::ExportJson:
      return tr(STR_READING_STATS_EXPORTED);
    case StatsDataMode::ImportJson:
      return tr(STR_READING_STATS_IMPORTED);
    case StatsDataMode::ExportStoryGraph:
      return tr(STR_STORYGRAPH_EXPORTED);
  }
  return "";
}

const char* StatsDataActivity::failText() const {
  return mode == StatsDataMode::ImportJson ? tr(STR_IMPORT_FAILED) : tr(STR_EXPORT_FAILED);
}

void StatsDataActivity::onEnter() {
  Activity::onEnter();
  state = WARNING;
  requestUpdate();
}

void StatsDataActivity::onExit() { Activity::onExit(); }

void StatsDataActivity::runAction() {
  bool ok = false;
  switch (mode) {
    case StatsDataMode::ExportJson:
      ok = READING_STATS.exportToFile(JSON_EXPORT_PATH);
      break;
    case StatsDataMode::ImportJson:
      if (!Storage.exists(JSON_EXPORT_PATH)) {
        LOG_ERR("STATS_DATA", "No export at %s", JSON_EXPORT_PATH);
        state = NO_FILE;
        requestUpdate();
        return;
      }
      ok = READING_STATS.importFromFile(JSON_EXPORT_PATH);
      break;
    case StatsDataMode::ExportStoryGraph:
      ok = StoryGraphExport::exportToCsv(STORYGRAPH_EXPORT_PATH);
      break;
  }

  state = ok ? SUCCESS : FAILED;
  requestUpdate();
}

void StatsDataActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, titleText());

  if (state == WARNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, confirmText(), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  if (state == WORKING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, workingText());
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  const char* message = successText();
  if (state == FAILED) {
    message = failText();
  } else if (state == NO_FILE) {
    message = tr(STR_NO_READING_STATS_EXPORT);
  }
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, message, true, EpdFontFamily::BOLD);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}

void StatsDataActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state = WORKING;
      }
      requestUpdateAndWait();
      runAction();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == SUCCESS || state == FAILED || state == NO_FILE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }
}
