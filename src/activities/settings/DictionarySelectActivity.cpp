#include "DictionarySelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

void DictionarySelectActivity::onEnter() {
  Activity::onEnter();

  DictionaryRegistry::discover(dictionaries);

  // Select the configured dictionary, falling back to "None" (row 0) when the
  // stored folder is gone — a dictionary deleted off the SD card must not leave
  // the cursor pointing at a row that no longer exists.
  selectedIndex = 0;
  if (SETTINGS.dictionaryName[0] != '\0') {
    for (size_t i = 0; i < dictionaries.size(); ++i) {
      if (strncmp(dictionaries[i].name.c_str(), SETTINGS.dictionaryName, sizeof(SETTINGS.dictionaryName) - 1) == 0) {
        selectedIndex = static_cast<int>(i) + 1;
        break;
      }
    }
  }

  requestUpdate();
}

void DictionarySelectActivity::onExit() { Activity::onExit(); }

void DictionarySelectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), totalItems(), selectedIndex,
                               /*hasSubtitle=*/false, tappedIndex)) {
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

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems());
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems());
    requestUpdate();
  });

  const int pageItems = GUI.listGeometry(listRect(), 0, /*hasSubtitle=*/false).pageItems;
  if (TouchListNav::pageSwipe(mappedInput, totalItems(), pageItems, selectedIndex)) {
    requestUpdate();
    return;
  }
}

void DictionarySelectActivity::handleSelection() {
  {
    RenderLock lock(*this);
    if (selectedIndex == 0) {
      SETTINGS.dictionaryName[0] = '\0';
    } else {
      const auto& chosen = dictionaries[static_cast<size_t>(selectedIndex) - 1].name;
      strncpy(SETTINGS.dictionaryName, chosen.c_str(), sizeof(SETTINGS.dictionaryName) - 1);
      SETTINGS.dictionaryName[sizeof(SETTINGS.dictionaryName) - 1] = '\0';
    }
    // Value-change guarded inside saveToFile's dirty check; this screen is only
    // reachable by an explicit user choice, so it is not a hot write path.
    SETTINGS.saveToFile();
  }

  onBack();
}

// List body between the header and the button hints. Shared by render() and
// the loop()'s tap hit-testing so the two can never disagree.
Rect DictionarySelectActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void DictionarySelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DICTIONARY));

  const bool noneActive = SETTINGS.dictionaryName[0] == '\0';
  GUI.drawList(
      renderer, listRect(), totalItems(), selectedIndex,
      [this](int index) -> std::string {
        return index == 0 ? tr(STR_NONE_OPT) : dictionaries[static_cast<size_t>(index) - 1].name;
      },
      nullptr, nullptr,
      [this, noneActive](int index) -> std::string {
        if (index == 0) return noneActive ? tr(STR_SELECTED) : "";
        const bool active = !noneActive && strncmp(dictionaries[static_cast<size_t>(index) - 1].name.c_str(),
                                                   SETTINGS.dictionaryName, sizeof(SETTINGS.dictionaryName) - 1) == 0;
        return active ? tr(STR_SELECTED) : "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
