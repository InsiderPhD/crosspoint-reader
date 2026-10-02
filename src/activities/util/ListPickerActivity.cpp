#include "ListPickerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "util/TouchListNav.h"

void ListPickerActivity::onEnter() {
  Activity::onEnter();
  if (selectedIndex < 0 || selectedIndex >= itemCount) selectedIndex = 0;
  startIndex = selectedIndex;
  requestUpdate();
}

void ListPickerActivity::moveTo(const int index) {
  selectedIndex = index;
  if (onCursor) onCursor(selectedIndex);
  requestUpdate();
}

void ListPickerActivity::confirmSelection() {
  setResult(ListPickResult{selectedIndex});
  finish();
}

void ListPickerActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void ListPickerActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), itemCount, selectedIndex,
                               /*hasSubtitle=*/false, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      moveTo(tappedIndex);
      return;
    case TouchListNav::TapResult::Activated:
      moveTo(tappedIndex);
      confirmSelection();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmSelection();
    return;
  }

  buttonNavigator.onNextRelease([this] { moveTo(ButtonNavigator::nextIndex(selectedIndex, itemCount)); });
  buttonNavigator.onPreviousRelease([this] { moveTo(ButtonNavigator::previousIndex(selectedIndex, itemCount)); });
  buttonNavigator.onNextContinuous([this] { moveTo(ButtonNavigator::nextIndex(selectedIndex, itemCount)); });
  buttonNavigator.onPreviousContinuous([this] { moveTo(ButtonNavigator::previousIndex(selectedIndex, itemCount)); });

  // Full Touch: a vertical swipe turns a page, by exactly the rows drawList drew.
  const int pageItems = GUI.listGeometry(listRect(), 0, /*hasSubtitle=*/false).pageItems;
  int pagedIndex = selectedIndex;
  if (TouchListNav::pageSwipe(mappedInput, itemCount, pageItems, pagedIndex)) {
    moveTo(pagedIndex);
    return;
  }
}

// List body between the header and the button hints. Shared by render() and
// the loop()'s tap hit-testing so the two can never disagree.
Rect ListPickerActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void ListPickerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title);

  // The right-hand marker flags the value the picker opened on.
  GUI.drawList(
      renderer, listRect(), itemCount, selectedIndex, labelFn, nullptr, nullptr,
      [this](int index) { return index == startIndex ? tr(STR_SELECTED) : ""; }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
