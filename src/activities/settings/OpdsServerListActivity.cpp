#include "OpdsServerListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <memory>
#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsServerEditActivity.h"
#include "OpdsServerStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "util/OpdsDownloadFolder.h"
#include "util/TouchListNav.h"

void OpdsServerListActivity::onEnter() {
  Activity::onEnter();
  selectedRow = 0;
  requestUpdate();
}

int OpdsServerListActivity::rowCount() const {
  const auto count = static_cast<int>(OPDS_STORE.getCount());
  return (count < static_cast<int>(OpdsServerStore::MAX_SERVERS) ? count + 1 : count) + 1;
}

bool OpdsServerListActivity::isAddRow(const int row) const {
  return row == static_cast<int>(OPDS_STORE.getCount()) && row < rowCount() - 1;
}

// The shared default every catalog without its own folder uses. Stored
// normalised; empty (typed "" or "/") is the SD root, as upstream.
void OpdsServerListActivity::editDefaultFolder() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_OPDS_DEFAULT_DOWNLOAD_FOLDER),
                                              SETTINGS.opdsDownloadFolder, OpdsDownloadFolder::MAX_LENGTH,
                                              InputType::Text),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        std::string folder;
        if (!OpdsDownloadFolder::normalize(std::get<KeyboardResult>(result.data).text, folder)) {
          LOG_ERR("OPDS", "Invalid default download folder");
          return;
        }
        if (folder == "/") folder.clear();
        if (folder == SETTINGS.opdsDownloadFolder) return;
        snprintf(SETTINGS.opdsDownloadFolder, sizeof(SETTINGS.opdsDownloadFolder), "%s", folder.c_str());
        SETTINGS.saveToFile();
        requestUpdate();
      });
}

void OpdsServerListActivity::activateRow(const int row) {
  if (isFolderRow(row)) {
    editDefaultFolder();
    return;
  }
  const int index = isAddRow(row) ? -1 : row;
  startActivityForResult(std::make_unique<OpdsServerEditActivity>(renderer, mappedInput, index),
                         [this](const ActivityResult&) {
                           // The editor may have added or deleted a row.
                           if (selectedRow >= rowCount()) selectedRow = rowCount() - 1;
                           requestUpdate();
                         });
}

void OpdsServerListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const int count = rowCount();
  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), count, selectedRow, /*hasSubtitle=*/true, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedRow = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      selectedRow = tappedIndex;
      activateRow(selectedRow);
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateRow(selectedRow);
    return;
  }

  buttonNavigator.onNextRelease([this, count] {
    selectedRow = ButtonNavigator::nextIndex(selectedRow, count);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, count] {
    selectedRow = ButtonNavigator::previousIndex(selectedRow, count);
    requestUpdate();
  });
}

// Shared by render() and loop()'s tap hit-testing so the two can never disagree.
Rect OpdsServerListActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void OpdsServerListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_OPDS_SERVERS));

  GUI.drawList(
      renderer, listRect(), rowCount(), selectedRow,
      [this](int index) {
        if (isFolderRow(index)) return std::string(tr(STR_OPDS_DEFAULT_DOWNLOAD_FOLDER));
        if (isAddRow(index)) return std::string(tr(STR_ADD_OPDS_SERVER));
        const OpdsServer* server = OPDS_STORE.getServer(static_cast<size_t>(index));
        return server ? server->label() : std::string();
      },
      [this](int index) {
        if (isFolderRow(index)) {
          return std::string(SETTINGS.opdsDownloadFolder[0] ? SETTINGS.opdsDownloadFolder : tr(STR_OPDS_SD_ROOT));
        }
        if (isAddRow(index)) return std::string();
        const OpdsServer* server = OPDS_STORE.getServer(static_cast<size_t>(index));
        // The URL under a named server; an unnamed one already shows it as its title.
        return server && !server->name.empty() ? server->url : std::string();
      },
      nullptr, nullptr, false);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
