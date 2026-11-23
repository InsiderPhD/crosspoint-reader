#include "BookFusionSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "BookFusionAuthActivity.h"
#include "BookFusionTokenStore.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RefreshBookFusionMetadataActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

namespace {
// Row order on screen.
enum Row : int {
  ROW_LINK = 0,
  ROW_UNLINK,
  ROW_FOLDER,
  ROW_FILE_NAME,
  ROW_SYNC_HIGHLIGHTS,
  ROW_SYNC_READING_TIME,
  ROW_REFRESH_DATA
};
static_assert(ROW_REFRESH_DATA + 1 == BookFusionSettingsActivity::MENU_ITEMS, "menuNames must cover every row");

constexpr StrId menuNames[BookFusionSettingsActivity::MENU_ITEMS] = {
    StrId::STR_BF_LINK_ACCOUNT,    StrId::STR_BF_UNLINK,          StrId::STR_BF_FOLDER,
    StrId::STR_BF_FILE_NAME,       StrId::STR_BF_SYNC_HIGHLIGHTS, StrId::STR_BF_SYNC_READING_TIME,
    StrId::STR_REFRESH_BF_METADATA};

// Value labels, in enum order (CrossPointSettings::BOOKFUSION_FOLDER / BOOKFUSION_FILE_NAME).
constexpr StrId folderNames[CrossPointSettings::BOOKFUSION_FOLDER_COUNT] = {
    StrId::STR_BF_FOLDER_ROOT, StrId::STR_BF_FOLDER_AUTHOR, StrId::STR_BF_FOLDER_BOOKFUSION,
    StrId::STR_BF_FOLDER_BOOKFUSION_AUTHOR};
constexpr StrId fileNameNames[CrossPointSettings::BOOKFUSION_FILE_NAME_COUNT] = {StrId::STR_BF_NAME_TITLE_AUTHOR,
                                                                                 StrId::STR_BF_NAME_TITLE};
}  // namespace

void BookFusionSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void BookFusionSettingsActivity::onExit() { Activity::onExit(); }

void BookFusionSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), MENU_ITEMS, static_cast<int>(selectedIndex),
                               /*hasSubtitle=*/false, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedIndex = static_cast<size_t>(tappedIndex);
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      selectedIndex = static_cast<size_t>(tappedIndex);
      handleSelection();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  // A press moves one row, a hold turns a page, as in every other list.
  const int itemCount = static_cast<int>(MENU_ITEMS);
  const int pageItems = GUI.listGeometry(listRect(), static_cast<int>(selectedIndex), /*hasSubtitle=*/false).pageItems;
  buttonNavigator.onNextPress([this, itemCount] {
    selectedIndex = static_cast<size_t>(ButtonNavigator::nextIndex(static_cast<int>(selectedIndex), itemCount));
    requestUpdate();
  });

  buttonNavigator.onPreviousPress([this, itemCount] {
    selectedIndex = static_cast<size_t>(ButtonNavigator::previousIndex(static_cast<int>(selectedIndex), itemCount));
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, itemCount, pageItems] {
    selectedIndex =
        static_cast<size_t>(ButtonNavigator::nextPageIndex(static_cast<int>(selectedIndex), itemCount, pageItems));
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, itemCount, pageItems] {
    selectedIndex =
        static_cast<size_t>(ButtonNavigator::previousPageIndex(static_cast<int>(selectedIndex), itemCount, pageItems));
    requestUpdate();
  });
}

void BookFusionSettingsActivity::handleSelection() {
  switch (static_cast<Row>(selectedIndex)) {
    case ROW_LINK:
      startActivityForResult(std::make_unique<BookFusionAuthActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
      return;
    case ROW_UNLINK:
      BF_TOKEN_STORE.clearToken();
      requestUpdate();
      return;
    // Each press cycles to the next value; the parent saves on return.
    case ROW_FOLDER:
      SETTINGS.bookfusionFolder =
          static_cast<uint8_t>((SETTINGS.bookfusionFolder + 1) % CrossPointSettings::BOOKFUSION_FOLDER_COUNT);
      requestUpdate();
      return;
    case ROW_FILE_NAME:
      SETTINGS.bookfusionFileName =
          static_cast<uint8_t>((SETTINGS.bookfusionFileName + 1) % CrossPointSettings::BOOKFUSION_FILE_NAME_COUNT);
      requestUpdate();
      return;
    case ROW_SYNC_HIGHLIGHTS:
      SETTINGS.bookfusionSyncHighlights = SETTINGS.bookfusionSyncHighlights ? 0 : 1;
      requestUpdate();
      return;
    case ROW_SYNC_READING_TIME:
      SETTINGS.bookfusionSyncReadingTime = SETTINGS.bookfusionSyncReadingTime ? 0 : 1;
      requestUpdate();
      return;
    case ROW_REFRESH_DATA:
      // Re-downloads covers, shelves and positions for every BookFusion book
      // on the card. It confirms before touching the network.
      startActivityForResult(std::make_unique<RefreshBookFusionMetadataActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
      return;
  }
}

// List body between the header and the button hints. Shared by render() and
// the loop()'s tap hit-testing so the two can never disagree.
Rect BookFusionSettingsActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void BookFusionSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BF_SYNC));

  GUI.drawList(
      renderer, listRect(), MENU_ITEMS, static_cast<int>(selectedIndex),
      [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr, nullptr,
      [](int index) -> std::string {
        switch (static_cast<Row>(index)) {
          case ROW_LINK:
            return BF_TOKEN_STORE.hasToken() ? std::string(tr(STR_BF_LINKED)) : std::string(tr(STR_BF_NOT_LINKED));
          case ROW_UNLINK:
            return "";
          case ROW_FOLDER:
            return std::string(I18N.get(folderNames[SETTINGS.bookfusionFolder]));
          case ROW_FILE_NAME:
            return std::string(I18N.get(fileNameNames[SETTINGS.bookfusionFileName]));
          case ROW_SYNC_HIGHLIGHTS:
            return std::string(SETTINGS.bookfusionSyncHighlights ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
          case ROW_SYNC_READING_TIME:
            return std::string(SETTINGS.bookfusionSyncReadingTime ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
          case ROW_REFRESH_DATA:
            return "";
        }
        return "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
