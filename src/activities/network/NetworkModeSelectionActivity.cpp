#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "BookFusionTokenStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

namespace {
// One row of the File Transfer menu.
struct MenuRow {
  NetworkMode mode;
  StrId title;
  StrId description;
  UIIcon icon;
};

constexpr MenuRow MENU_ROWS[] = {
    {NetworkMode::JOIN_NETWORK, StrId::STR_JOIN_NETWORK, StrId::STR_JOIN_DESC, UIIcon::Wifi},
    {NetworkMode::CONNECT_CALIBRE, StrId::STR_CALIBRE_WIRELESS, StrId::STR_CALIBRE_DESC, UIIcon::Library},
    {NetworkMode::CREATE_HOTSPOT, StrId::STR_CREATE_HOTSPOT, StrId::STR_HOTSPOT_DESC, UIIcon::Hotspot},
    {NetworkMode::BOOKFUSION, StrId::STR_BF_BROWSE_LIBRARY, StrId::STR_BF_LIBRARY_DESC, UIIcon::BookFusion},
    {NetworkMode::OPDS, StrId::STR_OPDS_BROWSER, StrId::STR_OPDS_DESC, UIIcon::Library},
};
constexpr int MAX_MENU_ITEM_COUNT = static_cast<int>(sizeof(MENU_ROWS) / sizeof(MENU_ROWS[0]));

// The BookFusion entry only appears once the user has linked an account in
// Settings → System → BookFusion Sync. Hiding it pre-link avoids the dead row
// that would otherwise just fail with NO_TOKEN once the user tried to use it.
bool rowVisible(const MenuRow& row) {
  if (row.mode == NetworkMode::BOOKFUSION) return BF_TOKEN_STORE.hasToken();
  // Same for OPDS: until a server URL is set in Settings there is nothing to browse.
  if (row.mode == NetworkMode::OPDS) return SETTINGS.opdsServerUrl[0] != '\0';
  return true;
}

// Collect the indices of the visible rows into `out`, returning how many there
// are. A conditional row used to be handled by simply truncating the list,
// which silently breaks as soon as another row follows it -- so the mapping
// from screen position to row is made explicit instead.
int visibleRows(int (&out)[MAX_MENU_ITEM_COUNT]) {
  int count = 0;
  for (int i = 0; i < MAX_MENU_ITEM_COUNT; i++) {
    if (rowVisible(MENU_ROWS[i])) out[count++] = i;
  }
  return count;
}

int visibleMenuItemCount() {
  int indices[MAX_MENU_ITEM_COUNT];
  return visibleRows(indices);
}

// The row shown at screen position `position`, or the first row if the
// selection is somehow out of range.
const MenuRow& rowAt(int position) {
  int indices[MAX_MENU_ITEM_COUNT];
  const int count = visibleRows(indices);
  if (position < 0 || position >= count) return MENU_ROWS[0];
  return MENU_ROWS[indices[position]];
}
}  // namespace

void NetworkModeSelectionActivity::onEnter() {
  Activity::onEnter();

  // Reset selection
  selectedIndex = 0;

  // Trigger first update
  requestUpdate();
}

void NetworkModeSelectionActivity::onExit() { Activity::onExit(); }

void NetworkModeSelectionActivity::loop() {
  // Handle back button - cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), visibleMenuItemCount(), selectedIndex,
                               /*hasSubtitle=*/true, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedIndex = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      selectedIndex = tappedIndex;
      handleSelection();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  // Handle confirm button - select current option
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  // Handle navigation
  const int itemCount = visibleMenuItemCount();
  buttonNavigator.onNext([this, itemCount] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, itemCount] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
    requestUpdate();
  });
}

void NetworkModeSelectionActivity::handleSelection() { onModeSelected(rowAt(selectedIndex).mode); }

// List body between the header and the button hints. Shared by render() and
// the loop()'s tap hit-testing so the two can never disagree.
Rect NetworkModeSelectionActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void NetworkModeSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FILE_TRANSFER));

  // Rows are addressed through rowAt(), so a hidden row (BookFusion, pre-link)
  // shifts the ones after it on screen without the labels and the selection
  // falling out of step.
  GUI.drawList(
      renderer, listRect(), visibleMenuItemCount(), selectedIndex,
      [](int index) { return std::string(I18N.get(rowAt(index).title)); },
      [](int index) { return std::string(I18N.get(rowAt(index).description)); },
      [](int index) { return rowAt(index).icon; });

  // Draw help text at bottom
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}

void NetworkModeSelectionActivity::onModeSelected(NetworkMode mode) {
  setResult(NetworkModeResult{mode});
  finish();
}

void NetworkModeSelectionActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
