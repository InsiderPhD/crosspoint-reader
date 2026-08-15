#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "BookFusionTokenStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

namespace {
// The BookFusion entry only appears once the user has linked an account in
// Settings → System → BookFusion Sync. Hiding it pre-link avoids the dead row
// that would otherwise just fail with NO_TOKEN once the user tried to use it.
constexpr int MAX_MENU_ITEM_COUNT = 4;
int visibleMenuItemCount() { return BF_TOKEN_STORE.hasToken() ? 4 : 3; }
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

void NetworkModeSelectionActivity::handleSelection() {
  NetworkMode mode = NetworkMode::JOIN_NETWORK;
  if (selectedIndex == 1) {
    mode = NetworkMode::CONNECT_CALIBRE;
  } else if (selectedIndex == 2) {
    mode = NetworkMode::CREATE_HOTSPOT;
  } else if (selectedIndex == 3) {
    mode = NetworkMode::BOOKFUSION;
  }
  onModeSelected(mode);
}

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

  // Menu items and descriptions. Arrays are sized to MAX_MENU_ITEM_COUNT but
  // only `visibleMenuItemCount()` of them are passed to drawList — the
  // BookFusion row at index 3 is hidden whenever the user hasn't linked an
  // account yet (see BookFusionTokenStore).
  static constexpr StrId menuItems[MAX_MENU_ITEM_COUNT] = {StrId::STR_JOIN_NETWORK, StrId::STR_CALIBRE_WIRELESS,
                                                           StrId::STR_CREATE_HOTSPOT, StrId::STR_BF_BROWSE_LIBRARY};
  static constexpr StrId menuDescs[MAX_MENU_ITEM_COUNT] = {StrId::STR_JOIN_DESC, StrId::STR_CALIBRE_DESC,
                                                           StrId::STR_HOTSPOT_DESC, StrId::STR_BF_LIBRARY_DESC};
  static constexpr UIIcon menuIcons[MAX_MENU_ITEM_COUNT] = {UIIcon::Wifi, UIIcon::Library, UIIcon::Hotspot,
                                                            UIIcon::BookFusion};

  GUI.drawList(
      renderer, listRect(), visibleMenuItemCount(), selectedIndex,
      [](int index) { return std::string(I18N.get(menuItems[index])); },
      [](int index) { return std::string(I18N.get(menuDescs[index])); }, [](int index) { return menuIcons[index]; });

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
