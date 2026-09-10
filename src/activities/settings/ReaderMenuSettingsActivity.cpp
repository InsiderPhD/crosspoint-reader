#include "ReaderMenuSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/reader/ReaderMenuVisibility.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

void ReaderMenuSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  isDirty = false;
  visibleRows.clear();
  visibleRows.reserve(ReaderMenuVisibility::kRowCount);
  for (uint8_t i = 0; i < ReaderMenuVisibility::kRowCount; i++) {
    if (ReaderMenuVisibility::isAvailable(ReaderMenuVisibility::kRows[i])) visibleRows.push_back(i);
  }

  requestUpdate();
}

void ReaderMenuSettingsActivity::onExit() { Activity::onExit(); }

void ReaderMenuSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // Batched rather than written per toggle: trimming a menu is a run of
    // toggles, and SPIFFS sectors have a finite erase count.
    if (isDirty) SETTINGS.saveToFile();
    finish();
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), rowCount(), selectedIndex,
                               /*hasSubtitle=*/false, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedIndex = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      handleSelection();
      requestUpdate();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    requestUpdate();
    return;
  }

  // Full Touch: a vertical swipe turns a page, matching the held side key.
  const int pageItems = GUI.listGeometry(listRect(), selectedIndex, /*hasSubtitle=*/false).pageItems;
  if (TouchListNav::pageSwipe(mappedInput, rowCount(), pageItems, selectedIndex)) {
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, rowCount());
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, rowCount());
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, rowCount());
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, rowCount());
    requestUpdate();
  });
}

void ReaderMenuSettingsActivity::handleSelection() {
  if (isShowAllRow(selectedIndex)) {
    ReaderMenuVisibility::showAll();
    isDirty = true;
    return;
  }

  const auto& row = ReaderMenuVisibility::kRows[visibleRows[selectedIndex]];
  ReaderMenuVisibility::setVisible(row.action, !ReaderMenuVisibility::isVisible(row.action));
  isDirty = true;
}

// List body between the header and the button hints. Shared by render() and
// the loop()'s tap hit-testing so the two can never disagree.
Rect ReaderMenuSettingsActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void ReaderMenuSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CUSTOMISE_READER_MENU));

  GUI.drawList(
      renderer, listRect(), rowCount(), selectedIndex,
      [this](int index) -> std::string {
        if (isShowAllRow(index)) return tr(STR_SHOW_ALL);
        return I18N.get(ReaderMenuVisibility::kRows[visibleRows[index]].label);
      },
      nullptr, nullptr,
      [this](int index) -> std::string {
        if (isShowAllRow(index)) return "";
        const auto& row = ReaderMenuVisibility::kRows[visibleRows[index]];
        return ReaderMenuVisibility::isVisible(row.action) ? tr(STR_SHOW) : tr(STR_HIDE);
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_SAVE_AND_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
