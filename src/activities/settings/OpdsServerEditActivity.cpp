#include "OpdsServerEditActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <memory>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "util/OpdsDownloadFolder.h"
#include "util/TouchListNav.h"

namespace {
constexpr StrId kRowLabels[] = {StrId::STR_OPDS_SERVER_NAME,
                                StrId::STR_CALIBRE_WEB_URL,
                                StrId::STR_USERNAME,
                                StrId::STR_PASSWORD,
                                StrId::STR_CUSTOM_HEADER_1,
                                StrId::STR_CUSTOM_HEADER_2,
                                StrId::STR_OPDS_DOWNLOAD_FOLDER,
                                StrId::STR_DELETE_OPDS_SERVER};
}  // namespace

OpdsServerEditActivity::OpdsServerEditActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const int index)
    : Activity("OpdsServerEdit", renderer, mappedInput), index(index) {}

void OpdsServerEditActivity::onEnter() {
  Activity::onEnter();
  const OpdsServer* existing = index >= 0 ? OPDS_STORE.getServer(static_cast<size_t>(index)) : nullptr;
  if (existing) {
    draft = *existing;
  } else {
    index = -1;
    draft = OpdsServer{};
  }
  dirty = false;
  selectedRow = 0;
  requestUpdate();
}

// One write per visit, and only for a real change. A new catalog without a URL
// is nothing to browse, so it is dropped rather than stored.
void OpdsServerEditActivity::saveAndFinish() {
  if (dirty) {
    if (index < 0) {
      if (!draft.url.empty() && !OPDS_STORE.addServer(draft)) LOG_ERR("OPDS", "Could not add OPDS server");
    } else if (!OPDS_STORE.updateServer(static_cast<size_t>(index), draft)) {
      LOG_ERR("OPDS", "Could not update OPDS server %d", index);
    }
  }
  finish();
}

void OpdsServerEditActivity::editText(const StrId title, std::string& field, const size_t maxLength,
                                      const InputType inputType) {
  // A URL field starts at "https://" to save typing; leaving just the scheme
  // clears it.
  const bool isUrl = inputType == InputType::Url;
  const std::string prefill = isUrl && field.empty() ? std::string("https://") : field;
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, I18N.get(title), prefill, maxLength, inputType),
      [this, &field, isUrl](const ActivityResult& result) {
        if (result.isCancelled) return;
        std::string text = std::get<KeyboardResult>(result.data).text;
        if (isUrl && (text == "https://" || text == "http://")) text.clear();
        if (text == field) return;
        field = std::move(text);
        dirty = true;
      });
}

// Typed as a path ("Books/OPDS" or "/Books/OPDS"); empty uses the shared
// default, "/" the SD root. A name FAT cannot hold is refused and the old
// value kept.
void OpdsServerEditActivity::editDownloadFolder() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_OPDS_DOWNLOAD_FOLDER), draft.downloadFolder,
                                              OpdsDownloadFolder::MAX_LENGTH, InputType::Text),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        std::string folder;
        if (!OpdsDownloadFolder::normalize(std::get<KeyboardResult>(result.data).text, folder)) {
          LOG_ERR("OPDS", "Invalid download folder");
          folderError = true;
          return;
        }
        folderError = false;
        if (folder == draft.downloadFolder) return;
        draft.downloadFolder = std::move(folder);
        dirty = true;
      });
}

void OpdsServerEditActivity::confirmDelete() {
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_OPDS_SERVER_CONFIRM), draft.label()),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        if (!OPDS_STORE.removeServer(static_cast<size_t>(index))) LOG_ERR("OPDS", "Could not delete server %d", index);
        finish();
      });
}

void OpdsServerEditActivity::handleSelection() {
  switch (selectedRow) {
    case ROW_NAME:
      editText(StrId::STR_OPDS_SERVER_NAME, draft.name, 63, InputType::Text);
      break;
    case ROW_URL:
      editText(StrId::STR_CALIBRE_WEB_URL, draft.url, 255, InputType::Url);
      break;
    case ROW_USERNAME:
      editText(StrId::STR_USERNAME, draft.username, 63, InputType::Text);
      break;
    case ROW_PASSWORD:
      editText(StrId::STR_PASSWORD, draft.password, 63, InputType::Password);
      break;
    case ROW_HEADER_1:
    case ROW_HEADER_2: {
      // Typed as "Name: Value"; an empty entry clears the slot.
      const size_t slot = selectedRow == ROW_HEADER_1 ? 0 : 1;
      startActivityForResult(std::make_unique<KeyboardEntryActivity>(
                                 renderer, mappedInput, I18N.get(kRowLabels[selectedRow]),
                                 formatHttpHeaderLine(draft.customHeaders[slot]), 256, InputType::Text),
                             [this, slot](const ActivityResult& result) {
                               if (result.isCancelled) return;
                               HttpHeader header;
                               parseHttpHeaderLine(std::get<KeyboardResult>(result.data).text, header);
                               HttpHeader& current = draft.customHeaders[slot];
                               if (header.name == current.name && header.value == current.value) return;
                               current = std::move(header);
                               dirty = true;
                             });
      break;
    }
    case ROW_DOWNLOAD_FOLDER:
      editDownloadFolder();
      break;
    case ROW_DELETE:
      confirmDelete();
      break;
    default:
      break;
  }
}

std::string OpdsServerEditActivity::rowValue(const int row) const {
  const auto orNotSet = [](const std::string& value) { return value.empty() ? std::string(tr(STR_NOT_SET)) : value; };
  switch (row) {
    case ROW_NAME:
      return orNotSet(draft.name);
    case ROW_URL:
      return orNotSet(draft.url);
    case ROW_USERNAME:
      return orNotSet(draft.username);
    case ROW_PASSWORD:
      return draft.password.empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
    case ROW_HEADER_1:
    case ROW_HEADER_2: {
      const std::string line = formatHttpHeaderLine(draft.customHeaders[row == ROW_HEADER_1 ? 0 : 1], true);
      return orNotSet(line);
    }
    case ROW_DOWNLOAD_FOLDER: {
      if (folderError) return std::string(tr(STR_OPDS_INVALID_FOLDER));
      if (draft.downloadFolder == "/") return std::string(tr(STR_OPDS_SD_ROOT));
      if (!draft.downloadFolder.empty()) return draft.downloadFolder;
      char buf[96];
      snprintf(buf, sizeof(buf), tr(STR_OPDS_USE_DEFAULT_FOLDER),
               SETTINGS.opdsDownloadFolder[0] ? SETTINGS.opdsDownloadFolder : tr(STR_OPDS_SD_ROOT));
      return std::string(buf);
    }
    default:
      return std::string();
  }
}

void OpdsServerEditActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    saveAndFinish();
    return;
  }

  const int count = rowCount();
  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), count, selectedRow, /*hasSubtitle=*/false, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedRow = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      selectedRow = tappedIndex;
      handleSelection();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
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

// List body between the sub-header (URL hint strip) and the button hints.
// Shared by render() and loop()'s tap hit-testing so the two can never disagree.
Rect OpdsServerEditActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + metrics.tabBarHeight;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void OpdsServerEditActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const std::string title = draft.label().empty() ? std::string(tr(STR_ADD_OPDS_SERVER)) : draft.label();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    tr(STR_CALIBRE_URL_HINT));

  GUI.drawList(
      renderer, listRect(), rowCount(), selectedRow, [](int row) { return std::string(I18N.get(kRowLabels[row])); },
      nullptr, nullptr, [this](int row) { return rowValue(row); }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_SAVE_AND_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
