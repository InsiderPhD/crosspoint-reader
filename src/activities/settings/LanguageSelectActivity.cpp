#include "LanguageSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>

#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

void LanguageSelectActivity::onEnter() {
  Activity::onEnter();

  // Set current selection based on current language
  const auto currentLang = static_cast<uint8_t>(I18N.getLanguage());
  const auto* begin = std::begin(SORTED_LANGUAGE_INDICES);
  const auto* end = std::end(SORTED_LANGUAGE_INDICES);
  const auto* it = std::find(begin, end, currentLang);
  selectedIndex = (it != end) ? std::distance(begin, it) : 0;

  requestUpdate();
}

void LanguageSelectActivity::onExit() { Activity::onExit(); }

void LanguageSelectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), totalItems, selectedIndex,
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

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(static_cast<int>(selectedIndex), totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(static_cast<int>(selectedIndex), totalItems);
    requestUpdate();
  });

  // Full Touch: a vertical swipe turns a page, matching the held side key.
  // pageItems comes from the theme's own windowing of listRect(), so a page
  // swipe always moves by exactly the rows drawList drew.
  const int pageItems = GUI.listGeometry(listRect(), 0, /*hasSubtitle=*/false).pageItems;
  if (TouchListNav::pageSwipe(mappedInput, totalItems, pageItems, selectedIndex)) {
    requestUpdate();
    return;
  }
}

void LanguageSelectActivity::handleSelection() {
  {
    RenderLock lock(*this);
    I18N.setLanguage(static_cast<Language>(SORTED_LANGUAGE_INDICES[selectedIndex]));
  }

  // Return to previous page
  onBack();
}

// List body between the header and the button hints. Shared by render() and
// the loop()'s tap hit-testing so the two can never disagree.
Rect LanguageSelectActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void LanguageSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LANGUAGE));

  // Current language marker
  const auto currentLang = static_cast<uint8_t>(I18N.getLanguage());
  GUI.drawList(
      renderer, listRect(), totalItems, selectedIndex,
      [this](int index) { return I18N.getLanguageName(static_cast<Language>(SORTED_LANGUAGE_INDICES[index])); },
      nullptr, nullptr,
      [this, currentLang](int index) { return SORTED_LANGUAGE_INDICES[index] == currentLang ? tr(STR_SELECTED) : ""; },
      true);

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
